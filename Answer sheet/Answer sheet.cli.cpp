// 答题卡识别 —— 命令行版本
//
// 与原始版本的差异仅在"外壳"：
//   * 接受命令行参数，而不是写死 slant_card.png
//   * 结果输出为 JSON 文件，而不是只画在图上弹窗
//   * imshow/waitKey 由 --gui 控制，默认关闭（可无头批处理）
//   * 中间结果统一写到 --debug-dir
//
// 检测与判分算法保持原样，一行未改。

#include <opencv2/opencv.hpp>
#include <opencv2/geometry.hpp>   // OpenCV 5: contourArea/arcLength/approxPolyDP 等
#include <iostream>
#include <vector>
#include <algorithm>
#include <string>
#include <map>
#include <cmath>
#include <utility>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#  define NOMINMAX
#  include <Windows.h>
#endif

// ─────────────────────────── 参数 ───────────────────────────
struct Options
{
    std::string imagePath;
    std::string outPath;
    std::string debugDir;
    bool gui = false;
};

static void printUsage()
{
    std::cout <<
        "usage: answersheet --image <path> [--out <result.json>]\n"
        "                   [--debug-dir <dir>] [--gui]\n"
        "\n"
        "  --image     输入图片路径（必填）\n"
        "  --out       结果 JSON 输出路径\n"
        "  --debug-dir 中间结果输出目录（校正图、二值图等）\n"
        "  --gui       显示窗口（默认关闭，便于批处理）\n";
}

// ─────────────────────── 透视校正（算法未改） ───────────────────────
static cv::Mat warp_perspective(const cv::Mat& image, const std::vector<cv::Point2f>& pts)
{
    std::vector<cv::Point2f> x_sorted(pts);
    std::sort(x_sorted.begin(), x_sorted.end(), [](const cv::Point2f& a, const cv::Point2f& b)
        {
            return a.x < b.x;
        });

    std::vector<cv::Point2f> left(x_sorted.begin(), x_sorted.begin() + 2);
    std::vector<cv::Point2f> right(x_sorted.begin() + 2, x_sorted.end());

    std::sort(left.begin(), left.end(), [](const cv::Point2f& a, const cv::Point2f& b)
        {
            return a.y < b.y;
        });

    cv::Point2f top_left = left[0];
    cv::Point2f bottom_left = left[1];

    std::sort(right.begin(), right.end(), [](const cv::Point2f& a, const cv::Point2f& b)
        {
            return a.y < b.y;
        });

    cv::Point2f top_right = right[0];
    cv::Point2f bottom_right = right[1];

    std::vector<cv::Point2f> src = { top_left, top_right, bottom_right, bottom_left };

    float width_bottom = std::sqrt((top_left.x - top_right.x) * (top_left.x - top_right.x) + (top_left.y - top_right.y) * (top_left.y - top_right.y));
    float width_top = std::sqrt((bottom_left.x - bottom_right.x) * (bottom_left.x - bottom_right.x) + (bottom_left.y - bottom_right.y) * (bottom_left.y - bottom_right.y));

    int new_width = std::max(static_cast<int>(width_bottom), static_cast<int>(width_top));

    float height_left = std::sqrt((top_left.x - bottom_left.x) * (top_left.x - bottom_left.x) + (top_left.y - bottom_left.y) * (top_left.y - bottom_left.y));
    float height_right = std::sqrt((top_right.x - bottom_right.x) * (top_right.x - bottom_right.x) + (top_right.y - bottom_right.y) * (top_right.y - bottom_right.y));

    int new_height = std::max(static_cast<int>(height_left), static_cast<int>(height_right));

    std::vector<cv::Point2f> dst =
    {
        cv::Point2f(0.f, 0.f),
        cv::Point2f(static_cast<float>(new_width - 1) , 0.f),
        cv::Point2f(static_cast<float>(new_width - 1) , static_cast<float>(new_height - 1)),
        cv::Point2f(0.f , static_cast<float>(new_height - 1)),
    };

    cv::Mat M = cv::getPerspectiveTransform(src, dst);

    cv::Mat Result;
    cv::warpPerspective(image, Result, M, cv::Size(new_width, new_height));
    return Result;
}

// ─────────────────────────── 结果结构 ───────────────────────────
struct SheetResult
{
    bool found = false;
    int total = 0;
    int correct = 0;
    int score = 0;
    std::vector<char> answers;        // 'A'..'D' 或 '?' 表示未判定
    std::vector<bool> isCorrect;
    std::string error;
};

// ─────────────────────── 核心处理（算法未改） ───────────────────────
static SheetResult process(const cv::Mat& img, const std::string& debugDir, cv::Mat& visOut)
{
    SheetResult result;

    // 1. 标准答案
    std::map<int, int> ANSWER = { {0,1} , {1,1} , {2,3} , {3,2} , {4,2} , {5,3} }; // B B D C C D
    std::map<int, std::string> answerDict = { {0,"A"},{1,"B"} ,{2,"C"} ,{3,"D"} };

    // 2. 预处理
    cv::Mat gray, gaussian, edged;
    cv::cvtColor(img, gray, cv::COLOR_BGR2GRAY);
    cv::GaussianBlur(gray, gaussian, cv::Size(5, 5), 0);
    cv::Canny(gaussian, edged, 50, 200);

    // 3. 找外轮廓
    std::vector<std::vector<cv::Point>> contours;
    std::vector<cv::Vec4i> hierarchy;
    cv::findContours(edged, contours, hierarchy, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    std::sort(contours.begin(), contours.end(), [](const std::vector<cv::Point>& c1, const std::vector<cv::Point>& c2)
        {
            return cv::contourArea(c1) > cv::contourArea(c2);
        });

    std::cout << "contours: " << contours.size() << std::endl;

    int right_sum = 0;

    for (const auto& contour : contours)
    {
        double p = cv::arcLength(contour, true) * 0.02f;
        std::vector<cv::Point> approx;
        cv::approxPolyDP(contour, approx, p, true);

        if (approx.size() == 4 && cv::contourArea(approx) > 20000)
        {
            std::cout << "candidate quad, area: " << cv::contourArea(approx) << std::endl;

            std::vector<cv::Point2f> approx2f;
            for (const auto& pt : approx)
            {
                approx2f.push_back(cv::Point2f(static_cast<float>(pt.x), static_cast<float>(pt.y)));
            }

            cv::Mat card = warp_perspective(img, approx2f);
            cv::Mat card_gray = warp_perspective(gray, approx2f);

            if (!debugDir.empty())
            {
                cv::imwrite(debugDir + "/card_rectified.png", card);
                cv::imwrite(debugDir + "/card_gray.png", card_gray);
            }

            cv::Mat thresh;
            cv::threshold(card_gray, thresh, 0, 255, cv::THRESH_BINARY_INV | cv::THRESH_OTSU);
            if (!debugDir.empty()) cv::imwrite(debugDir + "/card_thresh.png", thresh);

            std::vector<std::vector<cv::Point>> contours_options;
            cv::findContours(thresh.clone(), contours_options, hierarchy, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

            // 4. 筛出选项
            std::vector<std::vector<cv::Point>> options;
            for (const auto& c : contours_options)
            {
                cv::Rect boundingBox = cv::boundingRect(c);

                int w = boundingBox.width;
                int h = boundingBox.height;
                float ar = static_cast<float>(w) / static_cast<float>(h);

                double contour_area = cv::contourArea(c);
                double rect_area = w * h;
                double extent = contour_area / rect_area;

                int min_size = std::max(15, card_gray.cols / 40);

                if (w >= min_size && h >= min_size &&
                    ar >= 0.65f && ar <= 1.35f &&
                    contour_area > 200 && extent > 0.5)
                {
                    options.push_back(c);
                }
            }

            if (options.size() < 4)
            {
                std::cout << " -> not a card, only " << options.size() << " options" << std::endl;
                continue;
            }

            std::sort(options.begin(), options.end(), [](const std::vector<cv::Point>& c1, const std::vector<cv::Point>& c2) {
                return cv::boundingRect(c1).y < cv::boundingRect(c2).y;
                });

            // 5. 分行
            std::vector<std::vector<std::vector<cv::Point>>> questions;
            std::vector<std::vector<cv::Point>> current_row;
            int current_y = cv::boundingRect(options[0]).y;
            int row_threshold = cv::boundingRect(options[0]).height / 2;

            for (const auto& opt : options)
            {
                int opt_y = cv::boundingRect(opt).y;

                if (std::abs(opt_y - current_y) <= row_threshold)
                {
                    current_row.push_back(opt);
                }
                else
                {
                    if (current_row.size() >= 4)
                    {
                        questions.push_back(current_row);
                    }
                    current_row.clear();
                    current_row.push_back(opt);
                    current_y = opt_y;
                }
            }

            if (current_row.size() >= 4) questions.push_back(current_row);

            int total_qs = std::min(static_cast<int>(questions.size()), static_cast<int>(ANSWER.size()));

            if (total_qs == 0) {
                std::cout << " -> row split failed" << std::endl;
                continue;
            }

            // 6. 判分
            bool allJudged = true;
            std::vector<char> answers;
            std::vector<bool> correctFlags;

            for (int index = 0; index < total_qs; index++)
            {
                std::vector<std::vector<cv::Point>>& cnts_quadrant = questions[index];

                std::sort(cnts_quadrant.begin(), cnts_quadrant.end(), [](const std::vector<cv::Point>& c1, const std::vector<cv::Point>& c2) {
                    return cv::boundingRect(c1).x < cv::boundingRect(c2).x;
                    });

                std::vector<std::pair<int, int>> ioptions;

                for (size_t k = 0; k < 4; ++k)
                {
                    cv::Rect bbox = cv::boundingRect(cnts_quadrant[k]);

                    cv::Mat mask = cv::Mat::zeros(card_gray.size(), CV_8U);

                    cv::Point center(bbox.x + bbox.width / 2, bbox.y + bbox.height / 2);

                    int radius = std::min(bbox.width, bbox.height) / 2 - 3;
                    if (radius <= 0) radius = 1;

                    cv::circle(mask, center, radius, 255, -1);

                    cv::Mat masked_thresh;
                    cv::bitwise_and(thresh, mask, masked_thresh);
                    int total = cv::countNonZero(masked_thresh);

                    ioptions.push_back(std::make_pair(total, static_cast<int>(k)));
                }

                std::sort(ioptions.begin(), ioptions.end(), [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
                    return a.first > b.first;
                    });

                int choice_num = ioptions[0].second;

                cv::Scalar color;
                bool ok = (ANSWER[index] == choice_num);
                if (ok)
                {
                    color = cv::Scalar(0, 255, 0);
                    right_sum += 1;
                }
                else
                {
                    color = cv::Scalar(0, 0, 255);
                }

                answers.push_back("ABCD"[choice_num]);
                correctFlags.push_back(ok);

                cv::Rect best_box = cv::boundingRect(cnts_quadrant[choice_num]);
                cv::circle(card, cv::Point(best_box.x + best_box.width / 2, best_box.y + best_box.height / 2), best_box.width / 2, color, 2);
            }

            // 7. 结果汇总
            std::string s1 = "total" + std::to_string(ANSWER.size());
            std::string s2 = "right" + std::to_string(right_sum);
            std::string s3 = "score" + std::to_string(right_sum * 100 / ANSWER.size());

            cv::putText(card, s3 + " (" + s2 + "/" + s1 + ")", cv::Point(20, 40), cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 0, 255), 2);

            result.found = true;
            result.total = static_cast<int>(ANSWER.size());
            result.correct = right_sum;
            result.score = right_sum * 100 / static_cast<int>(ANSWER.size());
            result.answers = answers;
            result.isCorrect = correctFlags;
            visOut = card;

            return result;
        }
    }

    result.error = "no answer sheet found";
    return result;
}

// ─────────────────────────── JSON 输出 ───────────────────────────
static void writeJson(const std::string& path, const SheetResult& r)
{
    std::ofstream os(path);
    os << "{\n";
    os << "  \"found\": " << (r.found ? "true" : "false") << ",\n";

    if (!r.found)
    {
        os << "  \"error\": \"" << r.error << "\",\n";
        os << "  \"answers\": [],\n";
        os << "  \"correct\": 0,\n";
        os << "  \"total\": 0,\n";
        os << "  \"score\": 0\n";
        os << "}\n";
        return;
    }

    os << "  \"answers\": [";
    for (size_t i = 0; i < r.answers.size(); ++i)
    {
        os << "\"" << r.answers[i] << "\"";
        if (i + 1 < r.answers.size()) os << ", ";
    }
    os << "],\n";

    os << "  \"correct_flags\": [";
    for (size_t i = 0; i < r.isCorrect.size(); ++i)
    {
        os << (r.isCorrect[i] ? "true" : "false");
        if (i + 1 < r.isCorrect.size()) os << ", ";
    }
    os << "],\n";

    os << "  \"correct\": " << r.correct << ",\n";
    os << "  \"total\": " << r.total << ",\n";
    os << "  \"score\": " << r.score << "\n";
    os << "}\n";
}

// ─────────────────────────── main ───────────────────────────
int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);

    Options opt;

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--image" && i + 1 < argc)      opt.imagePath = argv[++i];
        else if (a == "--out" && i + 1 < argc)   opt.outPath = argv[++i];
        else if (a == "--debug-dir" && i + 1 < argc) opt.debugDir = argv[++i];
        else if (a == "--gui")                   opt.gui = true;
        else if (a == "--help" || a == "-h")     { printUsage(); return 0; }
        else { std::cerr << "unknown argument: " << a << "\n"; printUsage(); return 2; }
    }

    if (opt.imagePath.empty()) { printUsage(); return 2; }

    cv::Mat img = cv::imread(opt.imagePath);
    if (img.empty())
    {
        std::cerr << "cannot read image: " << opt.imagePath << std::endl;
        return 3;
    }

    if (!opt.debugDir.empty())
    {
        std::filesystem::create_directories(opt.debugDir);
    }

    cv::Mat vis;
    SheetResult result = process(img, opt.debugDir, vis);

    if (!opt.debugDir.empty() && !vis.empty())
        cv::imwrite(opt.debugDir + "/result.png", vis);

    if (!opt.outPath.empty())
        writeJson(opt.outPath, result);

    if (result.found)
    {
        std::cout << "RESULT answers=";
        for (char c : result.answers) std::cout << c;
        std::cout << " correct=" << result.correct << "/" << result.total
                  << " score=" << result.score << std::endl;
    }
    else
    {
        std::cout << "RESULT not_found" << std::endl;
    }

    if (opt.gui && !vis.empty())
    {
        cv::imshow("score", vis);
        cv::waitKey(0);
        cv::destroyAllWindows();
    }

    return result.found ? 0 : 4;
}
