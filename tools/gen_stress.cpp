// 压力测试样本生成器
//
// 与 gen_samples.cpp 的区别：
//   gen_samples 产出 5 张"手挑场景"，人眼可控，用来做回归基线；
//   本工具产出 N 张"随机扰动"，用来量化鲁棒性（准确率 / 各扰动维度的退化曲线）。
//
// 输出：
//   <out>/stress_XXXX.png      场景图
//   <out>/manifest.json        每张图的真值 + 使用的扰动参数
//
// 用法：
//   gen_stress.exe <out_dir> <source_image> [N] [seed]
//
// 扰动维度（每次随机组合）：
//   angle  旋转 -15~+15 度
//   scale  缩放  0.45~1.10
//   pos    位置偏移 ±8%
//   gray   填涂灰度 60~180（模拟铅笔深浅）
//   blur   高斯模糊 sigma 0~1.6
//   noise  高斯噪声 sigma 0~6
//   light  亮度/对比度扰动（模拟光照不均）

#include <opencv2/opencv.hpp>
#include <opencv2/geometry.hpp>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdio>

// ─────────────────────── 复用 gen_samples 的找卡/找气泡逻辑 ───────────────────────
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

static std::vector<cv::Rect> find_bubbles(const cv::Mat& cardGray)
{
    cv::Mat thresh;
    cv::threshold(cardGray, thresh, 0, 255, cv::THRESH_BINARY_INV | cv::THRESH_OTSU);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(thresh.clone(), contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    const int xMin = static_cast<int>(cardGray.cols * 0.15);

    std::vector<cv::Rect> boxes;
    for (const auto& c : contours)
    {
        cv::Rect bb = cv::boundingRect(c);
        if (bb.x < xMin) continue;
        if (bb.width < 15 || bb.height < 15) continue;

        float ar = float(bb.width) / float(bb.height);
        double area = cv::contourArea(c);
        double extent = area / double(bb.width * bb.height);

        if (ar >= 0.65f && ar <= 1.35f && area > 200 && extent > 0.5)
            boxes.push_back(bb);
    }

    std::sort(boxes.begin(), boxes.end(), [](const cv::Rect& a, const cv::Rect& b) { return a.y < b.y; });

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
        if (canvas.empty()) cv::vconcat(piece, flipped, canvas);
        else cv::vconcat(canvas, flipped, canvas);
    }
    return canvas(cv::Rect(0, 0, W, H)).clone();
}

int main(int argc, char** argv)
{
    if (argc < 3) { std::cerr << "usage: gen_stress <out_dir> <source_image> [N] [seed]\n"; return 1; }
    const std::string outDir = argv[1];
    const std::string srcPath = argv[2];
    const int N = (argc > 3) ? std::atoi(argv[3]) : 40;
    const int seed = (argc > 4) ? std::atoi(argv[4]) : 20261109;

    cv::Mat src = cv::imread(srcPath);
    if (src.empty()) { std::cerr << "cannot read " << srcPath << "\n"; return 1; }

    // 1. 定位源图里的答题卡（源图 = slant_card.png）
    cv::Mat gray, blur, edged;
    cv::cvtColor(src, gray, cv::COLOR_BGR2GRAY);
    cv::GaussianBlur(gray, blur, cv::Size(5, 5), 0);
    cv::Canny(blur, edged, 50, 200);
    cv::morphologyEx(edged, edged, cv::MORPH_CLOSE,
                     cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3)));

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

    // 2. 校正 + 清空填涂，得到空卡
    cv::Mat card = warp_card(src, quadF);
    cv::Mat cardGray;
    cv::cvtColor(card, cardGray, cv::COLOR_BGR2GRAY);

    std::vector<cv::Rect> bubbles = find_bubbles(cardGray);
    if (bubbles.size() != 24) { std::cerr << "expected 24 bubbles, got " << bubbles.size() << "\n"; return 1; }

    cv::Mat blank = card.clone();
    for (const auto& b : bubbles)
    {
        cv::Point c(b.x + b.width / 2, b.y + b.height / 2);
        int r = std::max(b.width, b.height) / 2;
        cv::circle(blank, c, r + 2, cv::Scalar(255, 255, 255), -1);
        cv::circle(blank, c, r - 2, cv::Scalar(0, 0, 0), 3);
    }

    cv::Mat bg = make_background(src, src.cols, src.rows);
    cv::RNG rng(static_cast<uint64_t>(seed));

    std::ofstream manifest(outDir + "/manifest.json");
    manifest << "{\n  \"seed\": " << seed << ",\n  \"count\": " << N << ",\n  \"cases\": [\n";

    const char* alphabet = "ABCD";
    int written = 0;

    for (int i = 0; i < N; ++i)
    {
        // ── 随机真值：每题 A~D，8% 概率留空（空白题要能判成 null） ──
        std::string truth;
        for (int q = 0; q < 6; ++q)
            truth += (rng.uniform(0, 100) < 8) ? '.' : alphabet[rng.uniform(0, 4)];

        // ── 随机扰动参数 ──
        double angle = rng.uniform(-15.0, 15.0);

        // 缩放上限受"旋转后的包围盒必须整卡落在画幅内"约束：
        //   W_rot = w*cos + h*sin，H_rot = w*sin + h*cos
        //   15° 时 W_rot = 767*0.966 + 894*0.259 = 972 > 900，即 s=1.0 就会转出画幅。
        // 出画幅的卡轮廓不成四边形，属于"相机没框全"的无效场景，不该出现在压力集里。
        double rad = std::abs(angle) * CV_PI / 180.0;
        double wRot = card.cols * std::cos(rad) + card.rows * std::sin(rad);
        double hRot = card.cols * std::sin(rad) + card.rows * std::cos(rad);
        double scaleMax = std::min(src.cols * 0.92 / wRot, src.rows * 0.92 / hRot);
        double scale = std::min(rng.uniform(0.45, 1.10), scaleMax);

        // 位移上限同样要按旋转后的实际占位算，否则 0.8*900=720 的卡再平移 8% 就出画幅。
        // 出画幅的卡轮廓不闭合，approxPolyDP 拿不到 4 个顶点，属于"相机没框全"的无效场景。
        double boxW = wRot * scale;
        double boxH = hRot * scale;
        double maxDx = std::max(0.0, (src.cols * 0.97 - boxW) / 2.0 / src.cols);
        double maxDy = std::max(0.0, (src.rows * 0.97 - boxH) / 2.0 / src.rows);

        double dx = rng.uniform(-maxDx, maxDx);
        double dy = rng.uniform(-maxDy, maxDy);
        int    fillGray = rng.uniform(60, 180);
        double blurSigma = rng.uniform(0.0, 1.6);
        double noiseSigma = rng.uniform(0.0, 6.0);
        double alpha = rng.uniform(0.80, 1.20);   // 对比度
        int    beta = rng.uniform(-30, 30);       // 亮度

        // ── 画填涂 ──
        cv::Mat drawn = blank.clone();
        for (int q = 0; q < 6; ++q)
        {
            int opt = -1;
            switch (truth[q]) { case 'A': opt = 0; break; case 'B': opt = 1; break;
                                case 'C': opt = 2; break; case 'D': opt = 3; break; default: break; }
            if (opt < 0) continue;

            cv::Rect b = bubbles[q * 4 + opt];
            cv::Point c(b.x + b.width / 2, b.y + b.height / 2);
            cv::circle(drawn, c, std::max(std::max(b.width, b.height) / 2 - 3, 1),
                       cv::Scalar(fillGray, fillGray, fillGray), -1);
        }

        // ── 反变换到场景 ──
        cv::Point2f cardCenter(card.cols / 2.f, card.rows / 2.f);
        cv::Point2f target(src.cols / 2.f + float(dx * src.cols), src.rows / 2.f + float(dy * src.rows));

        cv::Mat M = cv::getRotationMatrix2D(cardCenter, angle, scale);
        M.at<double>(0, 2) += target.x - cardCenter.x;
        M.at<double>(1, 2) += target.y - cardCenter.y;

        cv::Mat warped;
        cv::warpAffine(drawn, warped, M, src.size(), cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));

        cv::Mat mask = cv::Mat(drawn.size(), CV_8U, cv::Scalar(255));
        cv::Mat warpedMask;
        cv::warpAffine(mask, warpedMask, M, src.size(), cv::INTER_NEAREST, cv::BORDER_CONSTANT, cv::Scalar(0));

        cv::Mat scene = bg.clone();
        warped.copyTo(scene, warpedMask);

        // ── 成像退化 ──
        if (blurSigma > 0.05) cv::GaussianBlur(scene, scene, cv::Size(0, 0), blurSigma);
        scene.convertTo(scene, -1, alpha, beta);
        if (noiseSigma > 0.05)
        {
            // 注意：OpenCV 5.0 的 cv::randn / rng.fill(NORMAL) 在多通道 CV_16SC3 上有 bug
            //   —— 实测 ch0 完全没填（std=0），ch1/ch2 是垃圾值（mean≈-22000）。
            // 所以噪声必须先生成单通道，再逐通道相加。
            cv::Mat noise1(scene.size(), CV_16SC1);
            cv::randn(noise1, 0.0, noiseSigma);

            std::vector<cv::Mat> chs;
            cv::split(scene, chs);
            for (auto& c : chs)
            {
                cv::Mat c16;
                c.convertTo(c16, CV_16S);
                c16 += noise1;
                c16.convertTo(c, CV_8U);
            }
            cv::merge(chs, scene);
        }

        char name[64];
        std::snprintf(name, sizeof(name), "stress_%04d.png", i);
        cv::imwrite(outDir + "/" + name, scene);

        manifest << "    { \"image\": \"" << name << "\", \"answers\": [";
        for (int q = 0; q < 6; ++q)
        {
            if (truth[q] == '.') manifest << "null";
            else manifest << "\"" << truth[q] << "\"";
            if (q < 5) manifest << ", ";
        }
        manifest << "], \"perturb\": { \"angle\": " << angle
                 << ", \"scale\": " << scale
                 << ", \"fillGray\": " << fillGray
                 << ", \"blurSigma\": " << blurSigma
                 << ", \"noiseSigma\": " << noiseSigma
                 << ", \"alpha\": " << alpha
                 << ", \"beta\": " << beta << " } }";
        manifest << (i + 1 < N ? ",\n" : "\n");

        ++written;
    }

    manifest << "  ]\n}\n";
    manifest.close();

    std::cout << "[gen_stress] wrote " << written << " samples + manifest.json to " << outDir << "\n";
    return 0;
}
