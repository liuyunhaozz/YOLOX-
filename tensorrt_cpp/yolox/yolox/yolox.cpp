﻿// yolox
// 2021-08-16

#include <fstream>
#include <iostream>
#include <sstream>
#include <numeric>
#include <chrono>
#include <vector>
#include <opencv2/opencv.hpp>
#include "dirent.h"
#include "NvInfer.h"
#include "cuda_runtime_api.h"
#include "logging.h"

#define CHECK(status) \
    do\
    {\
        auto ret = (status);\
        if (ret != 0)\
        {\
            std::cerr << "Cuda failure: " << ret << std::endl;\
            abort();\
        }\
    } while (0)

#define DEVICE 0  // GPU id
#define NMS_THRESH 0.45
#define BBOX_CONF_THRESH 0.70

using namespace nvinfer1;

// stuff we know about the network and the input/output blobs
static const int INPUT_W = 640;
static const int INPUT_H = 640;
const char* INPUT_BLOB_NAME = "input_0";
const char* OUTPUT_BLOB_NAME = "output_0";
static Logger gLogger;


// yolox resize 和YOLO v5相似
cv::Mat static_resize(cv::Mat& img) {
	float r = min(INPUT_W / (img.cols*1.0), INPUT_H / (img.rows*1.0));
	// r = std::min(r, 1.0f);
	int unpad_w = r * img.cols;
	int unpad_h = r * img.rows;
	cv::Mat re(unpad_h, unpad_w, CV_8UC3);
	cv::resize(img, re, re.size());
	cv::Mat out(INPUT_W, INPUT_H, CV_8UC3, cv::Scalar(114, 114, 114));
	re.copyTo(out(cv::Rect(0, 0, re.cols, re.rows)));
	return out;
}

// 识别结果的结构体
struct Object
{
	cv::Rect_<float> rect;
	int label;
	float prob;
};

// 生成 grid和stride
struct GridAndStride
{
	int grid0;
	int grid1;
	int stride;
};

static void generate_grids_and_stride(const int target_size, std::vector<int>& strides, std::vector<GridAndStride>& grid_strides)
{
	for (auto stride : strides)
	{
		int num_grid = target_size / stride;
		for (int g1 = 0; g1 < num_grid; g1++)
		{
			for (int g0 = 0; g0 < num_grid; g0++)
			{
				grid_strides.push_back(GridAndStride { g0, g1, stride });
			}
		}
	}
}



//后处理
static inline float intersection_area(const Object& a, const Object& b)
{
	cv::Rect_<float> inter = a.rect & b.rect;
	return inter.area();
}

static void qsort_descent_inplace(std::vector<Object>& faceobjects, int left, int right)
{
	int i = left;
	int j = right;
	float p = faceobjects[(left + right) / 2].prob;

	while (i <= j)
	{
		while (faceobjects[i].prob > p)
			i++;

		while (faceobjects[j].prob < p)
			j--;

		if (i <= j)
		{
			// swap
			std::swap(faceobjects[i], faceobjects[j]);

			i++;
			j--;
		}
	}

#pragma omp parallel sections
	{
#pragma omp section
		{
			if (left < j) qsort_descent_inplace(faceobjects, left, j);
		}
#pragma omp section
		{
			if (i < right) qsort_descent_inplace(faceobjects, i, right);
		}
	}
}

static void qsort_descent_inplace(std::vector<Object>& objects)
{
	if (objects.empty())
		return;

	qsort_descent_inplace(objects, 0, objects.size() - 1);
}

static void nms_sorted_bboxes(const std::vector<Object>& faceobjects, std::vector<int>& picked, float nms_threshold)
{
	picked.clear();

	const int n = faceobjects.size();

	std::vector<float> areas(n);
	for (int i = 0; i < n; i++)
	{
		areas[i] = faceobjects[i].rect.area();
	}

	for (int i = 0; i < n; i++)
	{
		const Object& a = faceobjects[i];

		int keep = 1;
		for (int j = 0; j < (int)picked.size(); j++)
		{
			const Object& b = faceobjects[picked[j]];

			// intersection over union
			float inter_area = intersection_area(a, b);
			float union_area = areas[i] + areas[picked[j]] - inter_area;
			// float IoU = inter_area / union_area
			if (inter_area / union_area > nms_threshold)
				keep = 0;
		}

		if (keep)
			picked.push_back(i);
	}
}


static void generate_yolox_proposals(std::vector<GridAndStride> grid_strides, float* feat_blob, float prob_threshold, std::vector<Object>& objects)
{
	const int num_class = 8;  // 改成自己的类别

	const int num_anchors = grid_strides.size();

	for (int anchor_idx = 0; anchor_idx < num_anchors; anchor_idx++)
	{
		const int grid0 = grid_strides[anchor_idx].grid0;
		const int grid1 = grid_strides[anchor_idx].grid1;
		const int stride = grid_strides[anchor_idx].stride;

		const int basic_pos = anchor_idx * 13;   //13 = 8 + 5

		// yolox/models/yolo_head.py decode logic
		float x_center = (feat_blob[basic_pos + 0] + grid0) * stride;
		float y_center = (feat_blob[basic_pos + 1] + grid1) * stride;
		float w = exp(feat_blob[basic_pos + 2]) * stride;
		float h = exp(feat_blob[basic_pos + 3]) * stride;
		float x0 = x_center - w * 0.5f;
		float y0 = y_center - h * 0.5f;

		float box_objectness = feat_blob[basic_pos + 4];
		for (int class_idx = 0; class_idx < num_class; class_idx++)
		{
			float box_cls_score = feat_blob[basic_pos + 5 + class_idx];
			float box_prob = box_objectness * box_cls_score;
			if (box_prob > prob_threshold)
			{
				Object obj;
				obj.rect.x = x0;
				obj.rect.y = y0;
				obj.rect.width = w;
				obj.rect.height = h;
				obj.label = class_idx;
				obj.prob = box_prob;

				objects.push_back(obj);
			}

		} // class loop

	} // point anchor loop
}


// input blob
float* blobFromImage(cv::Mat& img) {
	cv::cvtColor(img, img, cv::COLOR_BGR2RGB);

	float* blob = new float[img.total() * 3];
	int channels = 3;
	int img_h = 640;
	int img_w = 640;
	std::vector<float> mean = { 0.485, 0.456, 0.406 };
	std::vector<float> std = { 0.229, 0.224, 0.225 };
	for (size_t c = 0; c < channels; c++)
	{
		for (size_t h = 0; h < img_h; h++)
		{
			for (size_t w = 0; w < img_w; w++)
			{
				blob[c * img_w * img_h + h * img_w + w] =
					(((float)img.at<cv::Vec3b>(h, w)[c]) / 255.0f - mean[c]) / std[c];
			}
		}
	}
	return blob;
}


//output blob  后处理
static void decode_outputs(float* prob, std::vector<Object>& objects, float scale, const int img_w, const int img_h) {
	std::vector<Object> proposals;
	std::vector<int> strides = { 8, 16, 32 };
	std::vector<GridAndStride> grid_strides;
	generate_grids_and_stride(INPUT_W, strides, grid_strides);
	generate_yolox_proposals(grid_strides, prob, BBOX_CONF_THRESH, proposals);
	std::cout << "num of boxes before nms: " << proposals.size() << std::endl;

	qsort_descent_inplace(proposals);

	std::vector<int> picked;
	nms_sorted_bboxes(proposals, picked, NMS_THRESH);


	int count = picked.size();

	std::cout << "num of boxes: " << count << std::endl;

	objects.resize(count);
	for (int i = 0; i < count; i++)
	{
		objects[i] = proposals[picked[i]];

		// adjust offset to original unpadded
		float x0 = (objects[i].rect.x) / scale;
		float y0 = (objects[i].rect.y) / scale;
		float x1 = (objects[i].rect.x + objects[i].rect.width) / scale;
		float y1 = (objects[i].rect.y + objects[i].rect.height) / scale;

		// clip
		x0 = max(min(x0, (float)(img_w - 1)), 0.f);
		y0 = max(min(y0, (float)(img_h - 1)), 0.f);
		x1 = max(min(x1, (float)(img_w - 1)), 0.f);
		y1 = max(min(y1, (float)(img_h - 1)), 0.f);

		objects[i].rect.x = x0;
		objects[i].rect.y = y0;
		objects[i].rect.width = x1 - x0;
		objects[i].rect.height = y1 - y0;
	}
}


// color 
const float color_list[8][3] =
{
	{0.000, 0.447, 0.741},
	{0.850, 0.325, 0.098},
	{0.929, 0.694, 0.125},
	{0.494, 0.184, 0.556},
	{0.466, 0.674, 0.188},
	{0.301, 0.745, 0.933},
	{0.635, 0.078, 0.184},
	{0.300, 0.300, 0.300}
};

// draw box
static void draw_objects(const cv::Mat& bgr, const std::vector<Object>& objects, std::string f)
{
	static const char* class_names[] = { "Liomyoma", "Lipoma", "Pancreatic Rest", "GIST", "Cyst",  "NET", "Cancer","Normal" };

	cv::Mat image = bgr.clone();

	for (size_t i = 0; i < objects.size(); i++)
	{
		const Object& obj = objects[i];

		fprintf(stderr, "%d = %.5f at %.2f %.2f %.2f x %.2f\n", obj.label, obj.prob,
			obj.rect.x, obj.rect.y, obj.rect.width, obj.rect.height);

		cv::Scalar color = cv::Scalar(color_list[obj.label][0], color_list[obj.label][1], color_list[obj.label][2]);
		float c_mean = cv::mean(color)[0];
		cv::Scalar txt_color;
		if (c_mean > 0.5) {
			txt_color = cv::Scalar(0, 0, 0);
		}
		else {
			txt_color = cv::Scalar(255, 255, 255);
		}

		cv::rectangle(image, obj.rect, color * 255, 2);

		char text[256];
		sprintf_s(text, "%s %.1f%%", class_names[obj.label], obj.prob * 100);

		int baseLine = 0;
		cv::Size label_size = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, 0.4, 1, &baseLine);

		cv::Scalar txt_bk_color = color * 0.7 * 255;

		int x = obj.rect.x;
		int y = obj.rect.y + 1;
		//int y = obj.rect.y - label_size.height - baseLine;
		if (y > image.rows)
			y = image.rows;
		//if (x + label_size.width > image.cols)
			//x = image.cols - label_size.width;

		cv::rectangle(image, cv::Rect(cv::Point(x, y), cv::Size(label_size.width, label_size.height + baseLine)),
			txt_bk_color, -1);

		cv::putText(image, text, cv::Point(x, y + label_size.height),
			cv::FONT_HERSHEY_SIMPLEX, 0.4, txt_color, 1);
	}

	cv::imwrite("det_res.jpg", image);
	fprintf(stderr, "save vis file\n");
	/* cv::imshow("image", image); */
	/* cv::waitKey(0); */
}


void doInference(IExecutionContext& context, float* input, float* output, const int output_size, cv::Size input_shape) {
	const ICudaEngine& engine = context.getEngine();

	// Pointers to input and output device buffers to pass to engine.
	// Engine requires exactly IEngine::getNbBindings() number of buffers.
	assert(engine.getNbBindings() == 2);
	void* buffers[2];

	// In order to bind the buffers, we need to know the names of the input and output tensors.
	// Note that indices are guaranteed to be less than IEngine::getNbBindings()
	const int inputIndex = engine.getBindingIndex(INPUT_BLOB_NAME);

	assert(engine.getBindingDataType(inputIndex) == nvinfer1::DataType::kFLOAT);
	const int outputIndex = engine.getBindingIndex(OUTPUT_BLOB_NAME);
	assert(engine.getBindingDataType(outputIndex) == nvinfer1::DataType::kFLOAT);
	int mBatchSize = engine.getMaxBatchSize();

	// Create GPU buffers on device
	CHECK(cudaMalloc(&buffers[inputIndex], 3 * input_shape.height * input_shape.width * sizeof(float)));
	CHECK(cudaMalloc(&buffers[outputIndex], output_size * sizeof(float)));

	// Create stream
	cudaStream_t stream;
	CHECK(cudaStreamCreate(&stream));

	// DMA input batch data to device, infer on the batch asynchronously, and DMA output back to host
	CHECK(cudaMemcpyAsync(buffers[inputIndex], input, 3 * input_shape.height * input_shape.width * sizeof(float), cudaMemcpyHostToDevice, stream));
	context.enqueue(1, buffers, stream, nullptr);
	CHECK(cudaMemcpyAsync(output, buffers[outputIndex], output_size * sizeof(float), cudaMemcpyDeviceToHost, stream));
	cudaStreamSynchronize(stream);

	// Release stream and buffers
	cudaStreamDestroy(stream);
	CHECK(cudaFree(buffers[inputIndex]));
	CHECK(cudaFree(buffers[outputIndex]));
}

int main(int argc, char** argv) {
	cudaSetDevice(DEVICE);  // 设置调用的GPU
	// create a model using the API directly and serialize it to a stream
	char *trtModelStream{ nullptr };
	size_t size{ 0 };

	// 读取engine file
	if (argc == 4 && std::string(argv[2]) == "-i") {
		const std::string engine_file_path{ argv[1] };
		std::ifstream file(engine_file_path, std::ios::binary);
		if (file.good()) {
			file.seekg(0, file.end);
			size = file.tellg();
			file.seekg(0, file.beg);
			trtModelStream = new char[size];
			assert(trtModelStream);
			file.read(trtModelStream, size);
			file.close();
		}
	}
	else {
		std::cerr << "arguments not right!" << std::endl;
		std::cerr << "run 'python3 yolox/deploy/trt.py -n yolox-{tiny, s, m, l, x}' to serialize model first!" << std::endl;
		std::cerr << "Then use the following command:" << std::endl;
		std::cerr << "./yolox ../model_trt.engine -i ../../../assets/dog.jpg  // deserialize file and run inference" << std::endl;
		return -1;
	}
	const std::string input_image_path{ argv[3] };

	//std::vector<std::string> file_names;
	//if (read_files_in_dir(argv[2], file_names) < 0) {
		//std::cout << "read_files_in_dir failed." << std::endl;
		//return -1;
	//}

	// 反序列化engine
	IRuntime* runtime = createInferRuntime(gLogger);
	assert(runtime != nullptr);
	ICudaEngine* engine = runtime->deserializeCudaEngine(trtModelStream, size);
	assert(engine != nullptr);
	IExecutionContext* context = engine->createExecutionContext();
	assert(context != nullptr);
	delete[] trtModelStream;
	auto out_dims = engine->getBindingDimensions(1);
	auto output_size = 1;
	for (int j = 0; j < out_dims.nbDims; j++) {
		output_size *= out_dims.d[j];
	}
	// 用于存储output blob
	static float* prob = new float[output_size];

	// 前处理
	cv::Mat img = cv::imread(input_image_path);
	int img_w = img.cols;
	int img_h = img.rows;
	cv::Mat pr_img = static_resize(img);
	std::cout << "blob image" << std::endl;

	float* blob;
	blob = blobFromImage(pr_img);
	float scale = min(INPUT_W / (img.cols*1.0), INPUT_H / (img.rows*1.0));

	// run inference
	auto start = std::chrono::system_clock::now();
	doInference(*context, blob, prob, output_size, pr_img.size());
	auto end = std::chrono::system_clock::now();
	std::cout << std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() << "ms" << std::endl;

	std::vector<Object> objects;
	decode_outputs(prob, objects, scale, img_w, img_h);
	draw_objects(img, objects, input_image_path);

	// destroy the engine
	context->destroy();
	engine->destroy();
	runtime->destroy();
	return 0;
}


