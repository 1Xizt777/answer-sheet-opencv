// 回归测试工具：合成答题卡样本。
//
// 用法：gen_samples.exe <输出目录> <源图片>
//
// 做法：
//   1. 用主程序同款流程定位 + 透视校正，得到正交的答题卡
//   2. 擦掉原有填涂，重画成空卡
//   3. 按指定答案填涂
//   4. 旋转/缩放后贴回"纯木纹"背景（背景由源图底部木纹镜像平铺合成，
//      不含原卡片的任何残影）
//   5. 写入 PNG + 同名 .expected.json

#include <opencv2/opencv.hpp>
#include <opencv2/geometry.hpp>   // OpenCV 5：contourArea/arcLength/approxPolyDP/getRotationMatrix2D 在此
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

static cv::Mat warp_card(const cv::Mat& image, const std::vector<cv::Point2f>& pts)
{
	std::vector<cv::Point2f> xs(pts);
	std::sort(xs.begin(), xs.end(), [](const cv::Point2f& a, const cv::Point2f& b) { return a.x < b.x; });

	std::vector<cv::Point2f> left(xs.begin(), xs.begin() + 2);
	std::vector<cv::Point2f> right(xs.begin() + 2, xs.end());
	std::sort(left.begin(), left.end(), [](const cv::Point2f& a, const cv::Point2f& b) { return a.y < b.y; });
	std::sort(right.begin(), right.end(), [](const cv::Point2f& a, const cv::Point2f& b) { return a.y < b.y; });

	cv::Point2f tl = left[0], bl = left[1], tr = right[0], br = right[1];
	std::vector<cv::Point2f> src = { tl, tr, br, bl };

	auto dist = [](const cv::Point2f& a, const cv::Point2f& b) {
		return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); };

	int W = std::max(static_cast<int>(dist(tl, tr)), static_cast<int>(dist(bl, br)));
	int H = std::max(static_cast<int>(dist(tl, bl)), static_cast<int>(dist(tr, br)));

	std::vector<cv::Point2f> dst = {
		{0.f, 0.f}, {float(W - 1), 0.f}, {float(W - 1), float(H - 1)}, {0.f, float(H - 1)} };

	cv::Mat out;
	cv::warpPerspective(image, out, cv::getPerspectiveTransform(src, dst), cv::Size(W, H));
	return out;
}

// 只在选项区域内找圆：排除左侧题号列（题号"6"会被误判成气泡）
static std::vector<cv::Rect> find_bubbles(const cv::Mat& cardGray)
{
	cv::Mat thresh;
	cv::threshold(cardGray, thresh, 0, 255, cv::THRESH_BINARY_INV | cv::THRESH_OTSU);

	std::vector<std::vector<cv::Point>> contours;
	cv::findContours(thresh.clone(), contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

	const int xMin = static_cast<int>(cardGray.cols * 0.15);   // 题号列在左侧 ~8%

	std::vector<cv::Rect> boxes;
	for (const auto& c : contours)
	{
		cv::Rect bb = cv::boundingRect(c);
		if (bb.x < xMin) continue;                       // 过滤题号
		if (bb.width < 15 || bb.height < 15) continue;

		float ar = float(bb.width) / float(bb.height);
		double area = cv::contourArea(c);
		double extent = area / double(bb.width * bb.height);

		if (ar >= 0.65f && ar <= 1.35f && area > 200 && extent > 0.5)
			boxes.push_back(bb);
	}

	std::sort(boxes.begin(), boxes.end(), [](const cv::Rect& a, const cv::Rect& b) { return a.y < b.y; });

	// 按 y 分行，每行恰好取 4 个
	std::vector<cv::Rect> ordered;
	for (size_t i = 0; i < boxes.size();)
	{
		int rowY = boxes[i].y;
		int tol = boxes[i].height / 2;
		std::vector<cv::Rect> row;
		size_t j = i;
		while (j < boxes.size() && std::abs(boxes[j].y - rowY) <= tol) { row.push_back(boxes[j]); ++j; }

		if (row.size() == 4)
		{
			std::sort(row.begin(), row.end(), [](const cv::Rect& a, const cv::Rect& b) { return a.x < b.x; });
			for (int k = 0; k < 4; ++k) ordered.push_back(row[k]);
		}
		i = (j > i) ? j : i + 1;
	}
	return ordered;
}

// 用源图底部的木纹镜像平铺出不含卡片的纯背景
static cv::Mat make_background(const cv::Mat& src, int W, int H)
{
	int y0 = int(src.rows * 0.76);
	cv::Mat patch = src(cv::Rect(0, y0, src.cols, src.rows - y0)).clone();
	if (patch.cols != W) { double sx = double(W) / patch.cols; cv::resize(patch, patch, cv::Size(W, int(patch.rows * sx))); }

	cv::Mat canvas;
	while (canvas.rows < H)
	{
		cv::Mat piece = patch;
		cv::Mat flipped;
		cv::flip(patch, flipped, 0);
		std::vector<cv::Mat> parts = { canvas, piece };
		if (canvas.empty()) cv::vconcat(piece, flipped, canvas);
		else cv::vconcat(canvas, flipped, canvas);
	}
	return canvas(cv::Rect(0, 0, W, H)).clone();
}

struct Sample
{
	std::string name;
	std::string answers;   // 6 个字符 A/B/C/D，'.' = 空白
	double angle;
	double scale;
	double dx, dy;         // 相对画面中心的偏移比例
	int gray;              // 填涂灰度
};

int main(int argc, char** argv)
{
	if (argc < 3) { std::cerr << "usage: gen_samples <out_dir> <source_image>\n"; return 1; }
	const std::string outDir = argv[1];
	const std::string srcPath = argv[2];

	cv::Mat src = cv::imread(srcPath);
	if (src.empty()) { std::cerr << "cannot read " << srcPath << "\n"; return 1; }

	// 1. 定位答题卡
	cv::Mat gray, blur, edged;
	cv::cvtColor(src, gray, cv::COLOR_BGR2GRAY);
	cv::GaussianBlur(gray, blur, cv::Size(5, 5), 0);
	cv::Canny(blur, edged, 50, 200);

	std::vector<std::vector<cv::Point>> contours;
	cv::findContours(edged, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
	std::sort(contours.begin(), contours.end(),
		[](const std::vector<cv::Point>& a, const std::vector<cv::Point>& b) {
			return cv::contourArea(a) > cv::contourArea(b); });

	std::vector<cv::Point> quad;
	for (const auto& c : contours)
	{
		std::vector<cv::Point> approx;
		cv::approxPolyDP(c, approx, cv::arcLength(c, true) * 0.02, true);
		if (approx.size() == 4 && cv::contourArea(approx) > 20000) { quad = approx; break; }
	}
	if (quad.empty()) { std::cerr << "card not found in source\n"; return 1; }

	std::vector<cv::Point2f> quadF;
	for (const auto& p : quad) quadF.emplace_back(float(p.x), float(p.y));

	// 2. 校正 + 清空填涂
	cv::Mat card = warp_card(src, quadF);
	cv::Mat cardGray;
	cv::cvtColor(card, cardGray, cv::COLOR_BGR2GRAY);

	std::vector<cv::Rect> bubbles = find_bubbles(cardGray);
	std::cout << "[gen] bubbles: " << bubbles.size() << "\n";
	if (bubbles.size() != 24) { std::cerr << "[gen] expected 24 bubbles, got " << bubbles.size() << "\n"; return 1; }

	cv::Mat blank = card.clone();
	for (const auto& b : bubbles)
	{
		cv::Point c(b.x + b.width / 2, b.y + b.height / 2);
		int r = std::max(b.width, b.height) / 2;
		cv::circle(blank, c, r + 2, cv::Scalar(255, 255, 255), -1);   // 擦掉填涂/字母
		cv::circle(blank, c, r - 2, cv::Scalar(0, 0, 0), 3);           // 重画边框
	}
	cv::imwrite(outDir + "/_card_blank.png", blank);

	// 3. 样本定义
	std::vector<Sample> samples = {
		{ "card_01_upright",   "BADBCD",  0.0,  1.00, 0.00,  0.00, 120 },
		{ "card_02_rotated",   "ABCDAB",  7.0,  0.85, 0.03, -0.02, 130 },
		{ "card_03_dark",      "DABCAD", -5.0,  0.90, -0.04, 0.03,  90 },
		{ "card_04_small",     "CCBBAA",  3.0,  0.55, 0.05,  0.06, 110 },
		{ "card_05_blank_row", "A.C.AD", -2.0,  0.95, 0.00,  0.04, 120 },
	};

	cv::Mat bg = make_background(src, src.cols, src.rows);

	for (const auto& s : samples)
	{
		// 填涂
		cv::Mat drawn = blank.clone();
		for (int q = 0; q < 6; ++q)
		{
			int opt = -1;
			switch (s.answers[q]) { case 'A': opt = 0; break; case 'B': opt = 1; break;
			                        case 'C': opt = 2; break; case 'D': opt = 3; break; default: break; }
			if (opt < 0) continue;                                   // '.' 保持空白

			cv::Rect b = bubbles[q * 4 + opt];
			cv::Point c(b.x + b.width / 2, b.y + b.height / 2);
			cv::circle(drawn, c, std::max(std::max(b.width, b.height) / 2 - 3, 1),
				cv::Scalar(s.gray, s.gray, s.gray), -1);
		}

		// 反变换：以卡片自身中心旋转缩放，再平移到目标位置
		cv::Point2f cardCenter(card.cols / 2.f, card.rows / 2.f);
		cv::Point2f target(src.cols / 2.f + s.dx * src.cols, src.rows / 2.f + s.dy * src.rows);

		cv::Mat M = cv::getRotationMatrix2D(cardCenter, s.angle, s.scale);
		M.at<double>(0, 2) += target.x - cardCenter.x;
		M.at<double>(1, 2) += target.y - cardCenter.y;

		cv::Mat warpedCard;
		cv::warpAffine(drawn, warpedCard, M, src.size(), cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));

		cv::Mat mask = cv::Mat(drawn.size(), CV_8U, cv::Scalar(255));
		cv::Mat warpedMask;
		cv::warpAffine(mask, warpedMask, M, src.size(), cv::INTER_NEAREST, cv::BORDER_CONSTANT, cv::Scalar(0));

		cv::Mat scene = bg.clone();
		warpedCard.copyTo(scene, warpedMask);

		cv::imwrite(outDir + "/" + s.name + ".png", scene);

		// 只输出"这张图应该被判成什么"，不写 .expected.json。
		// 期望文件由人维护并纳入版本控制；生成器若去覆盖它，
		// 会把这个文件的 baseline 段冲掉（曾发生过）。
		FILE* fp = fopen((outDir + "/" + s.name + ".answers.json").c_str(), "w");
		if (fp)
		{
			fprintf(fp, "{\n  \"image\": \"%s.png\",\n  \"answers\": [", s.name.c_str());
			for (int q = 0; q < 6; ++q)
			{
				if (s.answers[q] == '.') fprintf(fp, "null");
				else fprintf(fp, "\"%c\"", s.answers[q]);
				fprintf(fp, q < 5 ? ", " : "]\n}\n");
			}
			fclose(fp);
		}
		std::cout << "[gen] " << s.name << "  answers=" << s.answers << "\n";
	}

	return 0;
}
