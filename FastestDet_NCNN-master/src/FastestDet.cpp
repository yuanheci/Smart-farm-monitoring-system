#include <iostream>
#include <fstream>
#include <chrono>
#include <algorithm>
#include <vector>
#include <string>
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/opencv.hpp>
#include "ncnn/net.h"

#include <stdio.h>
#include <unistd.h>

using namespace std;
using namespace cv;

//mode = 0表示单独测试，mode = 1表示联合摄像头使用
#define mode 1

struct Object
{
    cv::Rect  rect;
    cv::Rect crop_rect;
    int label;
    float prob;
    int track_id;
    std::vector<cv::Point2d>landmark;
    std::vector<float>angle;
    float distance_to_my_car;
  //  std::vector<float>cal_angle;
    std::string class_name;  //类别标签
};

class FastestDet
{
public:
    FastestDet(const  char*  model_bin,const  char*  model_param);
    vector<Object> detect(Mat frame);
private:
    const int inpWidth = 512;
    const int inpHeight = 512;
    vector<string> class_names;
    int num_class;

    float confThreshold;
    float nmsThreshold;
    ncnn::Net _net;
    void drawPred(float conf, int left, int top, int right, int bottom, Mat& frame, int classid);
    void qsort_descent_inplace(std::vector<Object>& faceobjects);
    void qsort_descent_inplace(std::vector<Object>& faceobjects, int left, int right);
    inline float intersection_area(const Object& a, const Object& b);
    void nms_sorted_bboxes(const std::vector<Object>& faceobjects, std::vector<int>& picked, float nms_threshold);

};

FastestDet::FastestDet(const  char*  model_bin,const  char*  model_param)
{
    //  yolov5.opt.use_int8_inference=true;

      _net.opt.use_vulkan_compute = true;
      _net.opt.use_fp16_storage = true;
     //  yolov5.register_custom_layer("Swish", Swish_layer_creator);


      _net.load_param(model_param);
      _net.load_model(model_bin);

}


void FastestDet::nms_sorted_bboxes(const std::vector<Object>& faceobjects, std::vector<int>& picked, float nms_threshold)
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
inline float FastestDet::intersection_area(const Object& a, const Object& b)
{
    cv::Rect_<float> inter = a.rect & b.rect;
    return inter.area();
}

void FastestDet::qsort_descent_inplace(std::vector<Object>& faceobjects, int left, int right)
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

//貌似是只显示置信度最大的那个类别
void FastestDet::qsort_descent_inplace(std::vector<Object>& faceobjects)
{
    if (faceobjects.empty())
        return;

    qsort_descent_inplace(faceobjects, 0, faceobjects.size() - 1);
}

inline float sigmoid(float x)
{
    return 1.0 / (1 + expf(-x));
}

vector<Object> FastestDet::detect(Mat frame)
{
    std::vector<Object> proposals;
    std::vector<Object> objects;
    const int target_size = 512;
    const float prob_threshold = 0.25f;
    const float nms_threshold = 0.35f;

    const std::vector<std::string> class_names = {
        "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat", "traffic light",
        "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep", "cow",
        "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
        "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove", "skateboard", "surfboard",
        "tennis racket", "bottle", "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
        "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch",
        "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse", "remote", "keyboard", "cell phone",
        "microwave", "oven", "toaster", "sink", "refrigerator", "book", "clock", "vase", "scissors", "teddy bear",
        "hair drier", "toothbrush"
    };

    int img_w = frame.cols;
    int img_h = frame.rows;
   //  cv::resize(bgr, bgr, cv::Size(640, 640));
    ncnn::Mat inputImg = ncnn::Mat::from_pixels_resize(frame.data, ncnn::Mat::PIXEL_BGR,\
                                                       frame.cols, frame.rows, target_size, target_size);

    //Normalization of input image data
    const float mean_vals[3] = {0.f, 0.f, 0.f};
    const float norm_vals[3] = {1/255.f, 1/255.f, 1/255.f};
    inputImg.substract_mean_normalize(mean_vals, norm_vals);

    ncnn::Extractor ex = _net.create_extractor();
    ex.input("image_arrays", inputImg);
    ncnn::Mat out;
    ex.extract("outputs", out);
    int c =out.c;
    int w =out.w;
    int h = out.h;
    int num_class  =out.w -5;
    for (int i =0; i < c; i++)
    {
        for (int j = 0;j < h; j++)
        {
            const float *data_ptr =out.row(i *h+j);
            // find class index with max class score
            int class_index = 0;
            float class_score = -FLT_MAX;
            for (int k = 0; k < num_class; k++)
            {
                float score = data_ptr[5 + k];
                if (score > class_score)
                 {
                    class_index = k;
                    class_score = score;
                }
            }
            class_score *= data_ptr[0];
            if(class_score >0.8)
            {
                const int class_idx = class_index;
                float cx = (tanh(data_ptr[1]) + j) / (float)c;  ///cx
                float cy = (tanh(data_ptr[2]) + i) / (float)h;   ///cy
                float w = sigmoid(data_ptr[3]);   ///w
                float h = sigmoid(data_ptr[4]);  ///h

                cx *= float(frame.cols);
                cy *= float(frame.rows);
                w *= float(frame.cols);
                h *= float(frame.rows);

                int left = int(cx - 0.5 * w);
                int top = int(cy - 0.5 * h);
                cv::Rect rect =cv::Rect(left,top,w,h);

                Object obj;
                obj.prob =class_score;
                obj.rect =rect;
                obj.label =class_index;
                proposals.push_back(obj);
            }


        }

    }
    // sort all proposals by score from highest to lowest
    qsort_descent_inplace(proposals);

    // apply nms with nms_threshold
    std::vector<int> picked;
    nms_sorted_bboxes(proposals, picked, nms_threshold);

    int count = picked.size();

    objects.resize(count);

    bool flag = false;
    for (int i = 0; i < count; i++)
    {
        objects[i] = proposals[picked[i]];
        // 获取类别标签
        int class_index = objects[i].label;
        if (class_index >= 0 && class_index < class_names.size()) {
            objects[i].class_name = class_names[class_index];
        } else {
            objects[i].class_name = "Unknown"; // 处理类别索引超出范围的情况
        }

        // 计算文本的宽度
        int baseline = 0;
        cv::Size text_size = cv::getTextSize(objects[i].class_name, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseline);

        cv::rectangle(frame,objects[i].rect,cv::Scalar(0,255,0),1);
        cv::putText(frame, objects[i].class_name, cv::Point(objects[i].rect.x + objects[i].rect.width - text_size.width - 5, objects[i].rect.y - 5), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 0), 1);
	if(flag == false && objects[i].class_name == "chair"){
		flag = true;
		printf("chair");
	}
    }
//    cv::imshow("222",frame);
//    cv::waitKey(0);  
    cv::imwrite("Fast_res_image2.png", frame);

    // fprintf(stderr, "Processed image saved\n");

    //调用show_png程序将其展示到LCD

// #if mode
//     printf("start to show png...\n");
//     execl("./show_png_image", "show_png", "./Fast_res_image.png", NULL);
//     perror("execl"); // 只有当execl执行失败时才会执行到这里
//     exit(EXIT_FAILURE);
// #endif

    return objects;
 
}

const char* imagepath;

void test_fast_yolov2(void)
{
    cv::Mat src =cv::imread(imagepath, cv::IMREAD_UNCHANGED);
    cv::flip(src, src, 0);


#if mode   //mode = 1
    //配合摄像头使用
    FastestDet det("../ncnn/fastdet_512x512.bin",
                   "../ncnn/fastdet_512x512.param");

#else    //mode = 0, 单独测试（用于测试运行时间，可用time命令进行测试)
    FastestDet det("./fastdet_512x512.bin",
                   "./fastdet_512x512.param");

#endif
    vector<Object> res = det.detect(src);

}


int main(int argc, char *argv[])
{
   if(argc != 2){
	   fprintf(stderr, "Usage: %s [imagepath]\n", argv[0]);
	   return -1;
   } 
	imagepath = argv[1];
    test_fast_yolov2();
   return 1;

}



