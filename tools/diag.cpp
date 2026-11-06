// 诊断工具：打印答题卡定位过程中每一步的实际情况
#include <opencv2/opencv.hpp>
#include <opencv2/geometry.hpp>
#include <iostream>
#include <algorithm>
#include <vector>

int main(int argc, char** argv)
{
	std::string path = argc > 1 ? argv[1] : "slant_card.png";
	cv::Mat img = cv::imread(path);
	if (img.empty()) { std::cerr << "cannot read\n"; return 1; }

	cv::Mat gray, blur, edged;
	cv::cvtColor(img, gray, cv::COLOR_BGR2GRAY);
	cv::GaussianBlur(gray, blur, cv::Size(5, 5), 0);
	cv::Canny(blur, edged, 50, 200);

	std::vector<std::vector<cv::Point>> contours;
	cv::findContours(edged, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

	std::cout << "image: " << img.cols << "x" << img.rows << "\n";
	std::cout << "contours: " << contours.size() << "\n\n";

	struct Row { double area; size_t verts; double peri; double eps; };
	std::vector<Row> rows;
	for (const auto& c : contours)
	{
		std::vector<cv::Point> approx;
		double peri = cv::arcLength(c, true);
		double eps = peri * 0.02;
		cv::approxPolyDP(c, approx, eps, true);
		rows.push_back({ cv::contourArea(c), approx.size(), peri, eps });
	}
	std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.area > b.area; });

	std::cout << "top 12 contours by area:\n";
	std::cout << "  idx        area   verts     perim       eps  is_quad  big\n";
	for (size_t i = 0; i < std::min<size_t>(rows.size(), 12); ++i)
	{
		const auto& r = rows[i];
		printf("  %3zu  %10.1f  %5zu  %8.1f  %8.2f  %6s  %3s\n",
			i, r.area, r.verts, r.peri, r.eps,
			r.verts == 4 ? "yes" : "no",
			r.area > 20000 ? "yes" : "no");
	}
	return 0;
}
