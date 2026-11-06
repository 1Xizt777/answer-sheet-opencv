#include <opencv2/opencv.hpp>
#include <opencv2/geometry.hpp>
#include <iostream>
#include <vector>
#include <algorithm>
#include <string>
#include <Windows.h>
#include <map>
#include <cmath>
#include <utility>


//工具函数
cv::Mat warp_perspective(const cv::Mat& image, const std::vector<cv::Point2f>& pts)
{
	//对原图答题卡中的4个顶点按x进行排序，分为左侧两个顶点，右侧两个顶点
	std::vector<cv::Point2f> x_sorted(pts);
	std::sort(x_sorted.begin(), x_sorted.end(), [](const cv::Point2f& a, const cv::Point2f& b)
		{
			return a.x < b.x;	//按x升序排序
		});


	std::vector<cv::Point2f> left(x_sorted.begin(), x_sorted.begin() + 2);	//左侧两个顶点
	std::vector<cv::Point2f> right(x_sorted.begin() + 2, x_sorted.end());	//右侧两个顶点


	//对左侧两个顶点做排序，找出左上顶点，左下顶点
	std::sort(left.begin(), left.end(), [](const cv::Point2f& a, const cv::Point2f& b)
		{
			return a.y < b.y;
		});

	cv::Point2f top_left = left[0];		//左上顶点
	cv::Point2f bottom_left = left[1];	//左下顶点


	//对右侧两个顶点做排序，找出右上顶点，右下顶点
	std::sort(right.begin(), right.end(), [](const cv::Point2f& a, const cv::Point2f& b)
		{
			return a.y < b.y;
		});

	cv::Point2f top_right = right[0];	//右上顶点
	cv::Point2f bottom_right = right[1];	//右下顶点


	std::vector<cv::Point2f> src = { top_left, top_right, bottom_right, bottom_left };

	//计算校正后答题卡的新宽度（取长的）//点到点的距离公式
	float width_bottom = std::sqrt((top_left.x - top_right.x) * (top_left.x - top_right.x) + (top_left.y - top_right.y) * (top_left.y - top_right.y));
	float width_top = std::sqrt((bottom_left.x - bottom_right.x) * (bottom_left.x - bottom_right.x) + (bottom_left.y - bottom_right.y) * (bottom_left.y - bottom_right.y));

	//取两者中最大的
	int new_width = max(static_cast<int>(width_bottom), static_cast<int>(width_top));


	float height_left = std::sqrt((top_left.x - bottom_left.x) * (top_left.x - bottom_left.x) + (top_left.y - bottom_left.y) * (top_left.y - bottom_left.y));
	float height_right = std::sqrt((top_right.x - bottom_right.x) * (top_right.x - bottom_right.x) + (top_right.y - bottom_right.y) * (top_right.y - bottom_right.y));

	//取两者中最大的
	int new_height = max(static_cast<int>(height_left), static_cast<int>(height_right));


	//根据新宽度，新高度构造 校正后的答题卡图像的4个顶点
	std::vector<cv::Point2f> dst =
	{
		cv::Point2f(0.f,0.f),
		cv::Point2f(static_cast<float>(new_width - 1) , 0.f),
		cv::Point2f(static_cast<float>(new_width - 1) , static_cast<float>(new_height - 1)),
		cv::Point2f(0.f , static_cast<float>(new_height - 1)),
	};

	//使用getPerspectiveTransform构造转换矩阵
	cv::Mat M = cv::getPerspectiveTransform(src, dst);


	//使用warpPerspective矫正答题卡并裁边
	cv::Mat Result;
	cv::warpPerspective(image, Result, M, cv::Size(new_width, new_height));
	return Result;
}


//void final()
//{
//	//1.定义标准答案
//	std::map<int, int> ANSWER = { {0,1} , {1,1} , {2,3} , {3,2} , {4,2} , {5,3} }; // B B D C C D
//
//
//	//答案用的字典
//	std::map<int, std::string> answerDict = { {0,"A"},{1,"B"} ,{2,"C"} ,{3,"D"} };
//
//
//	//读入图像
//	cv::Mat img = cv::imread("slant_card.png");
//	if (img.empty())
//	{
//		std::cerr << "Error : cant not load image!" << std::endl;
//		return;
//	}
//
//
//	//2.预处理
//	cv::Mat gray, gaussian, edged;
//	cv::cvtColor(img, gray, cv::COLOR_BGR2GRAY);
//	cv::GaussianBlur(gray, gaussian, cv::Size(5, 5), 0);
//	cv::Canny(gaussian, edged, 50, 200);
//
//
//	//3.筛选出 矫正前图像 的所有选项轮廓中心点坐标
//	std::vector<std::vector<cv::Point>> contours;
//	std::vector<cv::Vec4i> hierarchy;
//	cv::findContours(edged, contours, hierarchy, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
//
//
//	//用轮廓中心点 获得 轮廓面积 降序排序
//	std::sort(contours.begin(), contours.end(), [](const std::vector<cv::Point>& c1 , const std::vector<cv::Point>& c2)
//		{
//			return cv::contourArea(c1) > cv::contourArea(c2);
//		});
//
//	std::cout << "总轮廓数：" << contours.size() << std::endl;
//
//
//	//对答题卡进行倾斜矫正，用于后续处理
//	int right_sum = 0;
//	for (const auto& contour : contours)
//	{
//		double p = cv::arcLength(contour,true) * 0.02f;
//		std::vector<cv::Point> approx;	//逼近后轮廓的顶点坐标
//		cv::approxPolyDP(contour, approx, p, true);
//
//		std::cout << "顶点个数：" << approx.size() << std::endl;
//
//		if (approx.size() == 4 && cv::contourArea(approx) > 20000)		//如果是四个顶点（答题卡四个顶点）
//		{
//
//			std::vector<cv::Point2f> approx2f;
//			for (const auto& p : approx)
//			{
//				approx2f.push_back(cv::Point2f(static_cast<float>(p.x), static_cast<float>(p.y)));
//			}
//
//			//获得矫正后的图像
//			cv::Mat card = warp_perspective(img, approx2f); //传入 原图 和 答题卡四个顶点 获得 card
//
//			cv::Mat card_gray = warp_perspective(gray, approx2f);
//
//			//反二值化
//			cv::Mat thresh;
//			cv::threshold(card_gray, thresh, 0, 255, cv::THRESH_BINARY_INV | cv::THRESH_OTSU);
//
//			
//			//获得 矫正后图像 的 所有轮廓中心点坐标
//			std::vector<std::vector<cv::Point>> contours_options;
//			cv::findContours(thresh.clone(), contours_options, hierarchy, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
//
//
//			//4.筛出所有选项(A,B,C,D) （通过外接矩形的宽高比ar去筛）
//			
//			std::vector<std::vector<cv::Point>> options;
//			for (const auto& contour : contours_options)
//			{
//				cv::Rect boundingBox = cv::boundingRect(contour);	//轮廓外接矩形
//
//				int w = boundingBox.width;		//外接矩形宽度
//				int h = boundingBox.height;		//外接矩形高度
//				float ar = static_cast<float>(w) / static_cast<float>(h);	//宽高比
//					
//
//				double contour_area = cv::contourArea(contour);
//				double rect_area = w * h;
//				double extent = contour_area / rect_area;
//
//
//
//				// 动态尺寸（防止透视变换后图像很小）
//				int min_size = max(20, card_gray.cols / 30);
//
//				if (w >= min_size && h >= min_size &&
//					ar >= 0.8f && ar <= 1.2f &&
//					contour_area > 300 && extent > 0.6)
//				{
//					options.push_back(contour);
//				}
//
//			}
//
//			if (options.size() < 4) continue;
//
//
//			std::sort(options.begin(), options.end(), [](const std::vector<cv::Point>& c1, const std::vector<cv::Point>& c2) {
//				return cv::boundingRect(c1).y < cv::boundingRect(c2).y;
//				});
//
//
//
//
//			std::vector<std::vector<std::vector<cv::Point>>> questions;
//			std::vector<std::vector<cv::Point>> current_row;
//			int current_y = cv::boundingRect(options[0]).y;
//			int row_threshold = cv::boundingRect(options[0]).height / 2; // 容差
//
//			for (const auto& opt : options) {
//				int opt_y = cv::boundingRect(opt).y;
//				if (std::abs(opt_y - current_y) <= row_threshold) {
//					current_row.push_back(opt);
//				}
//				else {
//					if (current_row.size() >= 4) { // 这一行至少有4个才被认定为有效题目
//						questions.push_back(current_row);
//					}
//					current_row.clear();
//					current_row.push_back(opt);
//					current_y = opt_y;
//				}
//			}
//			if (current_row.size() >= 4) questions.push_back(current_row);
//
//			// 6. 逐题判定得分
//			int total_qs = min(static_cast<int>(questions.size()), static_cast<int>(ANSWER.size()));
//			for (int index = 0; index < total_qs; index++)
//			{
//				
//				std::vector<std::vector<cv::Point>>& cnts_quadrant = questions[index];
//
//				// 行内按 X 坐标从左到右排序（确保顺序是 A, B, C, D）
//				std::sort(cnts_quadrant.begin(), cnts_quadrant.end(), [](const std::vector<cv::Point>& c1, const std::vector<cv::Point>& c2) {
//					return cv::boundingRect(c1).x < cv::boundingRect(c2).x;
//					});
//
//
//				// 只处理前 4 个（防止某一行多识别出干扰项）
//				std::vector<std::pair<int, int>> ioptions;
//
//
//
//				//单独处理每个选项
//				for (size_t k = 0; k < 4; ++k)
//				{
//					cv::Rect bbox = cv::boundingRect(cnts_quadrant[k]);
//					cv::Mat mask = cv::Mat::zeros(card_gray.size(), CV_8U);
//
//					// 用内缩的圆计算填涂面积，避免轮廓边缘线条的干扰
//					cv::Point center(bbox.x + bbox.width / 2, bbox.y + bbox.height / 2);
//					int radius = min(bbox.width, bbox.height) / 2 - 3;
//					cv::circle(mask, center, radius, 255, -1);
//
//					cv::Mat masked_thresh;
//					cv::bitwise_and(thresh, mask, masked_thresh);
//					int total = cv::countNonZero(masked_thresh);
//
//					ioptions.push_back(std::make_pair(total, static_cast<int>(k)));
//				}
//				
//
//				// 找出填涂面积最大的选项索引
//				std::sort(ioptions.begin(), ioptions.end(), [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
//					return a.first > b.first;
//					});
//
//
//
//				//白色最多的是填涂的选项
//				int choice_num = ioptions[0].second;
//
//				//确定选项值: ABCD
//				cv::Scalar color;
//
//				if (ANSWER[index] == choice_num)
//				{
//					color = cv::Scalar(0, 255, 0);
//					right_sum += 1;
//				}
//				else
//				{
//					color = cv::Scalar(0, 0, 255);
//				}
//
//				cv::Rect best_box = cv::boundingRect(cnts_quadrant[choice_num]);
//				cv::circle(card, cv::Point(best_box.x + best_box.width / 2, best_box.y + best_box.height / 2),best_box.width / 2, color, 2);
//			}
//				
//			//显示结果
//			std::string s1 = "total" + std::to_string(ANSWER.size());
//			std::string s2 = "right" + std::to_string(right_sum);
//			std::string s3 = "score" + std::to_string(right_sum * 100 / ANSWER.size()); // 百分制
//
//			cv::putText(card, s3 + " (" + s2 + "/" + s1 + ")", cv::Point(20, 40),cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 0, 255), 2);
//			cv::imshow("score", card);
//
//
//			// 成功处理完一张答题卡，跳出寻找答题卡的循环
//			break;
// 		}
//
//	}
//
//	cv::waitKey(0);
//	cv::destroyAllWindows();
//
//}

//正确版本
void final()
{
	//1.定义标准答案
	std::map<int, int> ANSWER = { {0,1} , {1,1} , {2,3} , {3,2} , {4,2} , {5,3} }; // B B D C C D

	//答案用的字典
	std::map<int, std::string> answerDict = { {0,"A"},{1,"B"} ,{2,"C"} ,{3,"D"} };

	//读入图像
	cv::Mat img = cv::imread("slant_card.png");
	if (img.empty())
	{
		std::cerr << "Error : cant not load image!" << std::endl;
		return;
	}

	//2.预处理
	cv::Mat gray, gaussian, edged;
	cv::cvtColor(img, gray, cv::COLOR_BGR2GRAY);
	cv::GaussianBlur(gray, gaussian, cv::Size(5, 5), 0);
	cv::Canny(gaussian, edged, 50, 200);


	//3.找外轮廓坐标点集
	std::vector<std::vector<cv::Point>> contours;
	std::vector<cv::Vec4i> hierarchy;
	cv::findContours(edged, contours, hierarchy, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);


	// 按面积降序(答题卡面积大概率最大)
	std::sort(contours.begin(), contours.end(), [](const std::vector<cv::Point>& c1, const std::vector<cv::Point>& c2)
		{
			return cv::contourArea(c1) > cv::contourArea(c2);
		});

	std::cout << "总轮廓数：" << contours.size() << std::endl;

	int right_sum = 0;	//正确数量

	bool card_found = false; // 标志位：是否找到了答题卡


	for (const auto& contour : contours)
	{
		double p = cv::arcLength(contour, true) * 0.02f;
		std::vector<cv::Point> approx;
		cv::approxPolyDP(contour, approx, p, true);		//多边形逼近

		// 如果顶点数是4，且面积足够大（>20000）
		if (approx.size() == 4 && cv::contourArea(approx) > 20000)
		{
			std::cout << "找到疑似答题卡四边形，面积: " << cv::contourArea(approx) << std::endl;

			std::vector<cv::Point2f> approx2f;
			for (const auto& pt : approx)
			{
				approx2f.push_back(cv::Point2f(static_cast<float>(pt.x), static_cast<float>(pt.y)));
			}

			//获得矫正后的图像
			cv::Mat card = warp_perspective(img, approx2f);
			cv::Mat card_gray = warp_perspective(gray, approx2f);

			//反二值化
			cv::Mat thresh;
			cv::threshold(card_gray, thresh, 0, 255, cv::THRESH_BINARY_INV | cv::THRESH_OTSU);

			//获得 矫正后图像 的所有轮廓坐标
			std::vector<std::vector<cv::Point>> contours_options;
			cv::findContours(thresh.clone(), contours_options, hierarchy, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

			//4.筛出所有圆形选项(A,B,C,D)
			std::vector<std::vector<cv::Point>> options; 
			for (const auto& c : contours_options)
			{
				cv::Rect boundingBox = cv::boundingRect(c);		//由轮廓点获得外接矩形

				int w = boundingBox.width;		//外接矩形宽度
				int h = boundingBox.height;		//外接矩形高度
				float ar = static_cast<float>(w) / static_cast<float>(h);	//宽高比

				double contour_area = cv::contourArea(c);	//轮廓面积	（轮廓面积通常小于或等于外接矩形面积）
				double rect_area = w * h;					//外接矩形面积
				double extent = contour_area / rect_area;		//占空比（它代表了这个形状把它的外接矩形“填满”了多少）


				// 动态尺寸（稍微调小一点阈值，防止过滤掉小圆）
				int min_size = max(15, card_gray.cols / 40);

				// 稍微放宽宽高比要求
				if (w >= min_size && h >= min_size &&		//contour_area用来确保它不是极小的噪点（比如 > 200）
					ar >= 0.65f && ar <= 1.35f &&			//w * h 和 ar（宽高比）用来确保它是个圆或正方形（比如 0.65f < ar < 1.35f）
					contour_area > 200 && extent > 0.5)		//extent 用来确保它是实心的，而不是文字或线条
				{
					options.push_back(c);
				}

			}

			// 如果当前四边形没筛出足够的圆，说明可能不是答题卡，跳过继续找
			if (options.size() < 4)
			{
				std::cout << " -> 当前四边形不是答题卡，筛出的选项仅: " << options.size() << " 个" << std::endl;
				// 调试时可以打开这行看看二值化对不对：
				// cv::imshow("Debug_thresh", thresh);
				// cv::waitKey(0);
				continue;
			}


			// 按该选项 外接矩形的 Y 坐标 从上到下 排序
			std::sort(options.begin(), options.end(), [](const std::vector<cv::Point>& c1, const std::vector<cv::Point>& c2) {
				return cv::boundingRect(c1).y < cv::boundingRect(c2).y;
				});



			//智能分行
			std::vector<std::vector<std::vector<cv::Point>>> questions;		// 最终存放所有题目的容器
			std::vector<std::vector<cv::Point>> current_row;			// 临时存放当前这一行的容器
			int current_y = cv::boundingRect(options[0]).y;			// 当前行的基准 Y 坐标
			int row_threshold = cv::boundingRect(options[0]).height / 2;		// 容差阈值：第一个选项的外接矩形的一半高度
																				//意思是：只要你的 Y 坐标和我基准线的差距不超过半个圆的高度，我就认为你俩在同一行

			for (const auto& opt : options) 
			{
				int opt_y = cv::boundingRect(opt).y;	//当前选项的外接矩形y坐标

				if (std::abs(opt_y - current_y) <= row_threshold)	//当前选项的外接矩形y坐标 - 当前行的基准y坐标 <= 容差阈值
				{
					current_row.push_back(opt);		//认为是同一行的选项，压入数组
				}
				else 
				{
					if (current_row.size() >= 4)	//不是同一行，那看看压满4个没有，压满了就是一道题
					{
						questions.push_back(current_row);		//压入一道题
					}

					current_row.clear();	//清空当前行数组，准备迎接新的一行

					current_row.push_back(opt);		//把当前这个属于新行的第一个选项放进去

					current_y = opt_y;		//更新基准 Y 坐标为这个新选项的 Y 坐标
				}

			}

			if (current_row.size() >= 4) questions.push_back(current_row);	//最后手动把最后剩下的这道题存进 questions 里




			// 6. 逐题判定得分
			int total_qs = min(static_cast<int>(questions.size()), static_cast<int>(ANSWER.size()));


			// 如果提取到的题目数量是 0，说明分行失败（选项没对齐）
			if (total_qs == 0) {
				std::cout << " -> 筛选到了选项，但分行失败！" << std::endl;
				continue;
			}

			for (int index = 0; index < total_qs; index++)
			{
				std::vector<std::vector<cv::Point>>& cnts_quadrant = questions[index];		//一道题的 四个选项的 轮廓点集

				// 行内按 X 坐标从左到右排序
				std::sort(cnts_quadrant.begin(), cnts_quadrant.end(), [](const std::vector<cv::Point>& c1, const std::vector<cv::Point>& c2) {
					return cv::boundingRect(c1).x < cv::boundingRect(c2).x; //排序完就是ABCD的顺序
					});

				std::vector<std::pair<int, int>> ioptions;

				for (size_t k = 0; k < 4; ++k)
				{
					cv::Rect bbox = cv::boundingRect(cnts_quadrant[k]); //单个选项（例如A)的外接矩形

					cv::Mat mask = cv::Mat::zeros(card_gray.size(), CV_8U);		//全是0，遮罩
						
					cv::Point center(bbox.x + bbox.width / 2, bbox.y + bbox.height / 2);	//外接矩形中心点

					int radius = min(bbox.width, bbox.height) / 2 - 3;
					// 防止半径变成负数导致 cv::circle 报错
					if (radius <= 0) radius = 1;

					cv::circle(mask, center, radius, 255, -1);		//画圆

					cv::Mat masked_thresh;
					cv::bitwise_and(thresh, mask, masked_thresh);		//thresh是反二值化完的结果，255是选项，0是背景
					int total = cv::countNonZero(masked_thresh);		//masked_thresh只保留了白色区域，也就是选项

					ioptions.push_back(std::make_pair(total, static_cast<int>(k)));
				}

				// 找出填涂面积最大的选项索引
				std::sort(ioptions.begin(), ioptions.end(), [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
					return a.first > b.first;
					});

				int choice_num = ioptions[0].second;	//填涂的索引

				cv::Scalar color;
				if (ANSWER[index] == choice_num)
				{
					color = cv::Scalar(0, 255, 0);
					right_sum += 1;
				}
				else
				{
					color = cv::Scalar(0, 0, 255);
				}

				cv::Rect best_box = cv::boundingRect(cnts_quadrant[choice_num]);
				cv::circle(card, cv::Point(best_box.x + best_box.width / 2, best_box.y + best_box.height / 2), best_box.width / 2, color, 2);
			}

			//显示结果
			std::string s1 = "total" + std::to_string(ANSWER.size());
			std::string s2 = "right" + std::to_string(right_sum);
			std::string s3 = "score" + std::to_string(right_sum * 100 / ANSWER.size());

			cv::putText(card, s3 + " (" + s2 + "/" + s1 + ")", cv::Point(20, 40), cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 0, 255), 2);

			cv::imshow("score", card);
			card_found = true;
			std::cout << "识别成功！已弹出图像。" << std::endl;
			break; // 成功处理完一张，跳出寻找答题卡的循环
		}
	}

	if (!card_found) {
		std::cout << "失败：整个图片中未找到符合条件的答题卡！" << std::endl;
	}

	cv::waitKey(0);
	cv::destroyAllWindows();
}

int main()
{
	SetConsoleOutputCP(CP_UTF8);

	final();

	return 0;
}

