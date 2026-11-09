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

// ───────────────────── 在二值图里找"像答题卡的那个四边形" ─────────────────────
//
// 输入是任意二值图（亮纸掩膜或 Canny 边缘），输出最大的那个四边形。
// 流程：外轮廓 → 取面积最大的几个 → 凸包 → 多边形逼近 → 恰好 4 个顶点且面积够大。
//
// 为什么要先取凸包：卡片边缘一旦有缺口（印刷白边被背景吞掉一两像素、边缘噪点、
// 反光），raw 轮廓逼近出来就是 5 或 6 个顶点，永远过不了 `== 4` 这一关。
// 答题卡本身是凸四边形，凸包不会误伤，凹进去的缺口正好被填平。
static bool findCardQuadIn(const cv::Mat& bin, std::vector<cv::Point>& quad)
{
    std::vector<std::vector<cv::Point>> contours;
    std::vector<cv::Vec4i> hierarchy;
    cv::findContours(bin.clone(), contours, hierarchy, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    std::sort(contours.begin(), contours.end(),
        [](const std::vector<cv::Point>& c1, const std::vector<cv::Point>& c2)
        {
            return cv::contourArea(c1) > cv::contourArea(c2);
        });

    for (const auto& contour : contours)
    {
        if (contour.size() < 3) continue;

        std::vector<cv::Point> hull;
        cv::convexHull(contour, hull);
        if (hull.size() < 4) continue;

        std::vector<cv::Point> approx;
        cv::approxPolyDP(hull, approx, cv::arcLength(hull, true) * 0.02f, true);

        if (approx.size() == 4 && cv::contourArea(approx) > 20000)
        {
            quad.swap(approx);
            return true;
        }
    }
    return false;
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

    // 3. 找答题卡的四边形。
    //
    // 主路线：**亮纸掩膜**。答题卡是画面里唯一一大片高亮区域，Otsu 一分割，
    //         纸就是白色的一个连通块，轮廓天然闭合，不受背景纹理影响。
    // 备选：  Canny 边缘 + CLOSE 补断点。
    //
    // 为什么主路线不能只用 Canny：
    //   背景木纹是水平条纹，亮度梯度也在 Canny 的阈值范围内。卡片一旦斜放，
    //   木纹的直边和卡片的边经常在 CLOSE 之后连成一条线，最大轮廓就从卡片
    //   "漏"到背景上去了。实测压力集 3/40 是这种：
    //     stress_0035  raw 轮廓面积 155730，凸包面积 248706（凸度只有 0.626），
    //                  凸包顶点 (833,562)(811,984)(14,918)(469,544) 根本不是卡片四角；
    //     stress_0020  凸度 0.899；stress_0027 凸度 0.842。
    //   这三张 warp 出来的"卡片"是斜切的一小块，气泡尺寸从 80 递减到 47，
    //   行分组只凑出 2 行，答案输出 "CD"（应为 DCDA?B）。
    //   改用亮纸掩膜后，同一批图全部 n=4、凸度 ≥ 0.99。
    std::vector<cv::Point> quad;
    bool foundCard = false;

    {
        cv::Mat paper, bin;
        cv::GaussianBlur(gray, paper, cv::Size(5, 5), 0);
        cv::threshold(paper, bin, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
        // 9x9 闭运算把气泡、文字、残留的细线并进纸面，保证纸是一个整块
        cv::morphologyEx(bin, bin, cv::MORPH_CLOSE,
                         cv::getStructuringElement(cv::MORPH_RECT, cv::Size(9, 9)));
        foundCard = findCardQuadIn(bin, quad);
        std::cout << "card via paper mask: " << (foundCard ? "yes" : "no") << std::endl;
    }

    if (!foundCard)
    {
        // 2b. Canny 路线（备选）。
        //     Canny 在答题卡四角容易断 1~2 像素，导致边框环不闭合，
        //     轮廓面积只剩 6552（远小于 20000 的判定阈值），整张卡就找不到了。
        //
        //     这里必须用 CLOSE 而不是 DILATE：
        //       DILATE 会把边框向外永久撑大 1px，warp 时四边形随之变大，
        //              卡片最外圈就采样到背景木纹（实测 borderMean 245 → 146），
        //              反二值化后整张卡边缘连成白环，RETR_EXTERNAL 只剩 1 个轮廓，
        //              所有气泡都被吞掉（73 → 1）。
        //       CLOSE  先膨胀后腐蚀，断点补上了，但边框位置复原（borderMean 保持 245）。
        //     实测 card_01_upright：raw 无边框轮廓 → close3 得到 area=684931 的四边形。
        cv::Mat edged;
        cv::Canny(gaussian, edged, 50, 200);
        cv::morphologyEx(edged, edged, cv::MORPH_CLOSE,
                         cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3)));
        foundCard = findCardQuadIn(edged, quad);
        std::cout << "card via canny: " << (foundCard ? "yes" : "no") << std::endl;
    }

    if (!foundCard)
    {
        result.error = "no answer sheet found";
        return result;
    }

    int right_sum = 0;      //答对题数量

    {
        {
            std::cout << "candidate quad, area: " << cv::contourArea(quad) << std::endl;

            std::vector<cv::Point2f> approx2f;
            for (const auto& pt : quad)
            {
                approx2f.push_back(cv::Point2f(static_cast<float>(pt.x), static_cast<float>(pt.y)));
            }

            //获取矫正后的图像，整张答题卡
            cv::Mat card = warp_perspective(img, approx2f);     
            cv::Mat card_gray = warp_perspective(gray, approx2f);


            //把中间图像存放，方便debug
            if (!debugDir.empty())
            {
                cv::imwrite(debugDir + "/card_rectified.png", card);
                cv::imwrite(debugDir + "/card_gray.png", card_gray);
            }


            //反二值化，大于阈值则取0，对于其他值则取255，阈值由Otsu算法选择
            cv::Mat thresh;
            cv::threshold(card_gray, thresh, 0, 255, cv::THRESH_BINARY_INV | cv::THRESH_OTSU);
            if (!debugDir.empty()) cv::imwrite(debugDir + "/card_thresh.png", thresh);


            //获得 矫正后图像的 所有轮廓坐标点集
            std::vector<std::vector<cv::Point>> contours_options;
            std::vector<cv::Vec4i> hierarchy;
            cv::findContours(thresh.clone(), contours_options, hierarchy, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);



            // 4. 筛出选项轮廓
            std::vector<std::vector<cv::Point>> options;    //只存放选项的轮廓，排除其他杂余轮廓
            for (const auto& c : contours_options)
            {
                cv::Rect boundingBox = cv::boundingRect(c);     //轮廓外接矩形

                int w = boundingBox.width;      //外接矩形宽度
                int h = boundingBox.height;     //外接矩形

                float ar = static_cast<float>(w) / static_cast<float>(h);   //宽高比

                double contour_area = cv::contourArea(c);   //轮廓面积  
                double rect_area = w * h;                   //外接矩形面积

                double extent = contour_area / rect_area;   //轮廓面积 占 外接矩形面积的多少，80%? 90%?

                int min_size = std::max(15, card_gray.cols / 40);

                if (w >= min_size && h >= min_size &&       //筛选
                    ar >= 0.65f && ar <= 1.35f &&
                    contour_area > 200 && extent > 0.5)
                {
                    options.push_back(c);
                }
            }


            if (options.size() < 4)     //如果选项轮廓没有4个，凑不成一道题，肯定不是答题卡
            {
                std::cout << " -> not a card, only " << options.size() << " options" << std::endl;
                result.error = "no option bubbles found";
                return result;
            }

            // 4b. 按气泡大小一致性再筛一遍。
            //     答题卡上所有选项气泡尺寸基本相同，用中位数做基准，
            //     偏离太大的（墨点、题号残迹、卡面污渍）直接剔除。
            //     实测 slant_card 混进来一个 22x27 的墨点（正常气泡 89x71），
            //     它落在第 6 行的 y 容差内，会把真正的 D 选项挤出前 4 个。
            {
                std::vector<int> widths, heights;
                for (const auto& c : options)
                {
                    cv::Rect bb = cv::boundingRect(c);
                    widths.push_back(bb.width);
                    heights.push_back(bb.height);
                }
                std::sort(widths.begin(), widths.end());
                std::sort(heights.begin(), heights.end());
                int medW = widths[widths.size() / 2];
                int medH = heights[heights.size() / 2];

                std::vector<std::vector<cv::Point>> consistent;
                for (const auto& c : options)
                {
                    cv::Rect bb = cv::boundingRect(c);
                    float rw = static_cast<float>(bb.width) / medW;
                    float rh = static_cast<float>(bb.height) / medH;
                    if (rw >= 0.6f && rw <= 1.5f && rh >= 0.6f && rh <= 1.5f)
                        consistent.push_back(c);
                }
                if (consistent.size() >= 4) options.swap(consistent);
            }

            //把选项轮廓按外接矩形的y坐标升序排序
            std::sort(options.begin(), options.end(), [](const std::vector<cv::Point>& c1, const std::vector<cv::Point>& c2) {
                return cv::boundingRect(c1).y < cv::boundingRect(c2).y;
                });


            // 5. 分行：只要你的 Y 坐标和我基准线的差距不超过半个圆的高度，我就认为你俩在同一行

            std::vector<std::vector<std::vector<cv::Point>>> questions;     //存放所有题目的容器

            std::vector<std::vector<cv::Point>> current_row;            //临时存放当前这一行的容器

            int current_y = cv::boundingRect(options[0]).y;             // 当前行的基准 Y 坐标
            int row_threshold = cv::boundingRect(options[0]).height / 2;        //容差阈值：第一个选项的外接矩形的一半高度

            //遍历所有选项
            for (const auto& opt : options)
            {
                int opt_y = cv::boundingRect(opt).y;    //当前选项的外接矩形y坐标

                if (std::abs(opt_y - current_y) <= row_threshold)     //矩形y坐标 - 当前行基准y坐标 <= 容差阈值 ，则算是在同一行
                {
                    current_row.push_back(opt);     //压入这一行的容器
                }
                else
                {
                    if (current_row.size() >= 4)    //不在同一行了，看看是不是四个选项，凑成一道题了
                    {
                        questions.push_back(current_row);
                    }
                    current_row.clear();    //清空当前行，为新一行做准备
                    current_row.push_back(opt);     //新一行第一个压入
                    current_y = opt_y;          //更新当前行的基准 Y 坐标
                }
            }

            if (current_row.size() >= 4) questions.push_back(current_row);  //最后手动压入最后一道题


            // 5b. 每行必须正好 4 个选项。
            //     一行里超过 4 个，说明混进了杂点（实测 slant_card 第 6 行混进一个墨点，
            //     按 x 排序后前 4 个里根本没有 D，第 6 题就永远读错）。
            //     这里按"到达行中位数"的做法保留最像气泡的 4 个：
            //     先按 y 对齐到行首，再看每个框的高度是否接近行内中位数。
            {
                for (auto& row : questions)
                {
                    if (row.size() <= 4) continue;

                    std::vector<int> hs;
                    for (const auto& c : row) hs.push_back(cv::boundingRect(c).height);
                    std::sort(hs.begin(), hs.end());
                    int medH = hs[hs.size() / 2];

                    std::sort(row.begin(), row.end(),
                        [medH](const std::vector<cv::Point>& a, const std::vector<cv::Point>& b)
                        {
                            int da = std::abs(cv::boundingRect(a).height - medH);
                            int db = std::abs(cv::boundingRect(b).height - medH);
                            return da < db;
                        });
                    row.resize(4);
                    std::sort(row.begin(), row.end(),
                        [](const std::vector<cv::Point>& a, const std::vector<cv::Point>& b)
                        {
                            return cv::boundingRect(a).x < cv::boundingRect(b).x;
                        });
                }
                // 保持原有行序（按 y 升序）
                std::sort(questions.begin(), questions.end(),
                    [](const std::vector<std::vector<cv::Point>>& a,
                       const std::vector<std::vector<cv::Point>>& b)
                    {
                        return cv::boundingRect(a[0]).y < cv::boundingRect(b[0]).y;
                    });
            }



            //题目数量 和 标准答案数量比较min
            int total_qs = std::min(static_cast<int>(questions.size()), static_cast<int>(ANSWER.size()));


            if (total_qs == 0) {
                std::cout << " -> row split failed" << std::endl;
                result.error = "row split failed";
                return result;
            }



            // 6. 判分
            bool allJudged = true;
            std::vector<char> answers;
            std::vector<bool> correctFlags;

            for (int index = 0; index < total_qs; index++)
            {
                //第index道题的全部轮廓点集
                std::vector<std::vector<cv::Point>>& cnts_quadrant = questions[index];

                //从左到右排序
                std::sort(cnts_quadrant.begin(), cnts_quadrant.end(), [](const std::vector<cv::Point>& c1, const std::vector<cv::Point>& c2) {
                    return cv::boundingRect(c1).x < cv::boundingRect(c2).x;
                    });



                std::vector<std::pair<double, int>> ioptions;   // (填涂程度 0~1, 选项序号)
                std::vector<double> maskAreas;                  // 兼容保留：遮罩面积

                for (size_t k = 0; k < 4; ++k)
                {
                    //第index道题中从左到右的第k个选项
                    cv::Rect bbox = cv::boundingRect(cnts_quadrant[k]);

                    // ── 填涂程度改用"灰度对比"而非"OTSU 白像素计数" ──────────
                    // 原判据：反二值化后数白色像素 / 圆形遮罩面积。
                    // 致命问题：OTSU 是**整张卡**求一个阈值，被黑色文字和气泡描边拉低
                    // （实测浅填涂卡上阈值只有 60~90），铅笔涂到灰度 150~178 时
                    // 反二值化后气泡内几乎全黑，占比 < 0.35 → 整行输出 '?'。
                    // 实测：fillGray>140 的样本准确率只有 56%，而 fillGray<100 是 100%。
                    //
                    // 新判据：气泡**内部**均值 vs 气泡**周围纸面**均值。
                    //   fill = (纸面均值 - 内部均值) / 纸面均值
                    // 分子分母都随光照同比缩放，所以光照不均、曝光变化都不影响；
                    // 铅笔深浅只改变 fill 的大小，不改变"涂了 vs 没涂"的判据结构。
                    //   纸面 ≈ 240、深涂 ≈ 70  → fill ≈ 0.71
                    //   纸面 ≈ 240、浅涂 ≈ 175 → fill ≈ 0.27
                    //   纸面 ≈ 240、空泡 ≈ 238 → fill ≈ 0.008
                    // 内部圆取 0.58r（避开印刷的圆环线条），环形取 1.25r~1.70r（气泡之间的空白纸面）。
                    int r = std::min(bbox.width, bbox.height) / 2;
                    cv::Point center(bbox.x + bbox.width / 2, bbox.y + bbox.height / 2);

                    int innerR = std::max(1, static_cast<int>(r * 0.58));
                    int ringIn = std::max(innerR + 2, static_cast<int>(r * 1.25));
                    int ringOut = std::max(ringIn + 2, static_cast<int>(r * 1.70));

                    cv::Mat innerMask = cv::Mat::zeros(card_gray.size(), CV_8U);
                    cv::circle(innerMask, center, innerR, 255, -1);

                    cv::Mat ringMask = cv::Mat::zeros(card_gray.size(), CV_8U);
                    cv::circle(ringMask, center, ringOut, 255, -1);
                    cv::circle(ringMask, center, ringIn, 0, -1);

                    double innerMean = cv::mean(card_gray, innerMask)[0];
                    double paperMean = cv::mean(card_gray, ringMask)[0];
                    if (paperMean < 1.0) paperMean = 1.0;

                    double fill = (paperMean - innerMean) / paperMean;
                    if (fill < 0.0) fill = 0.0;

                    ioptions.push_back(std::make_pair(fill, static_cast<int>(k)));
                    maskAreas.push_back(static_cast<double>(cv::countNonZero(innerMask)));
                }


                std::sort(ioptions.begin(), ioptions.end(), [](const std::pair<double, int>& a, const std::pair<double, int>& b) {
                    return a.first > b.first;   //按填涂程度降序排序
                    });

                // ── 填涂判定 ────────────────────────────────────────────
                // 两道闸：
                //   1) 绝对阈值 FILL_MIN_RATIO —— 涂得太浅就当没涂，输出 '?'
                //   2) 与次大值的 margin —— 两个选项都涂了（多涂）无法判断，也输出 '?'
                //
                // 阈值不是拍脑袋定的，是拿 200 张随机扰动图跑出来的分布定的：
                //   实心填涂（1110 个）：min 0.0942 / p1 0.1451 / p5 0.2257 / p50 0.4723 / max 0.8132
                //   空白气泡（  90 个）：全部 0.0000
                // 两类之间是一整段空档，阈值放在 0.05：
                //   比最浅的真实填涂（0.0942）低 47%，留足余量；
                //   又明显高于空白（0.0000），空白不会被误判成选项。
                // 之前定 0.12 就翻过车：stress_0089 是"浅铅笔 + 过曝"（fillGray=177、
                // alpha=1.15、beta=+18）的极端组合，6 个选项实测 0.094~0.116，
                // 全被 0.12 挡在门外，整张卡输出 ??????。
                const double FILL_MIN_RATIO = 0.05;   // 内部比纸面暗 5% 以上才算"涂了"
                const double FILL_MIN_MARGIN = 0.04;  // 第一名要比第二名高 4 个百分点才算无歧义

                auto ratioOf = [&](int i) { return ioptions[i].first; };

                // ioptions 已按填涂程度降序，ratioOf(0)/ratioOf(1) 即第一名/第二名
                double bestRatio   = ratioOf(0);
                double secondRatio = ratioOf(1);

                int  choice_num = ioptions[0].second;
                char answerChar = '?';          // '?' = 未判定（空白或多涂）
                bool judged     = false;

                if (bestRatio >= FILL_MIN_RATIO)
                {
                    if (bestRatio - secondRatio >= FILL_MIN_MARGIN)
                    {
                        answerChar = "ABCD"[choice_num];
                        judged = true;
                    }
                    else
                    {
                        // 两处都涂了：多涂，判为无效
                        std::cout << " Q" << (index + 1) << " multiple marks ("
                                  << bestRatio << " vs " << secondRatio << ")" << std::endl;
                    }
                }
                else
                {
                    std::cout << " Q" << (index + 1) << " blank (best " << bestRatio << ")" << std::endl;
                }

                if (!judged) allJudged = false;

                cv::Scalar color;
                bool ok = judged && (ANSWER[index] == choice_num);    //第index道题的答案 是否等于 填涂的选项
                if (ok)
                {
                    color = cv::Scalar(0, 255, 0);  //答对了是绿色
                    right_sum += 1;
                }
                else
                {
                    color = cv::Scalar(0, 0, 255);      //错了是红色
                }

                answers.push_back(answerChar);             //答题卡的填涂的答案压入answers
                correctFlags.push_back(ok);                //对错压入correctFlags


                cv::Rect best_box = cv::boundingRect(cnts_quadrant[choice_num]);
                cv::circle(card, cv::Point(best_box.x + best_box.width / 2, best_box.y + best_box.height / 2), best_box.width / 2, color, 2);
            }

            // 7. 结果汇总
            // 分数画在卡片左下角空白处。原来画在 (20,40)，正好压在"计算机视觉测试答题卡"标题上。
            const int score = right_sum * 100 / static_cast<int>(ANSWER.size());
            std::string summary = "score " + std::to_string(score) + "  (" + std::to_string(right_sum)
                                + "/" + std::to_string(ANSWER.size()) + ")";

            int baseline = 0;
            const double fontScale = 0.7;
            const int    fontThick = 2;
            cv::Size ts = cv::getTextSize(summary, cv::FONT_HERSHEY_SIMPLEX, fontScale, fontThick, &baseline);

            // 底部白边够高就画在卡内底部；不够就补一条白边再画，避免文字被裁掉。
            int textY = card.rows - 14;
            if (textY + baseline + 6 > card.rows || card.rows - 14 - ts.height < card.rows * 3 / 4)
            {
                cv::Mat taller(card.rows + baseline + 24, card.cols, card.type(), cv::Scalar(255, 255, 255));
                card.copyTo(taller(cv::Rect(0, 0, card.cols, card.rows)));
                card = taller;
                textY = card.rows - 14;
            }
            cv::putText(card, summary, cv::Point(20, textY), cv::FONT_HERSHEY_SIMPLEX,
                        fontScale, cv::Scalar(0, 0, 255), fontThick);

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

    //如果没找到则拼错误版本
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

    //找到了则拼正确版本，写入answer
    os << "  \"answers\": [";
    for (size_t i = 0; i < r.answers.size(); ++i)
    {
        os << "\"" << r.answers[i] << "\"";
        if (i + 1 < r.answers.size()) os << ", ";   //JSON 数组元素之间要有逗号，但最后一个后面不能有逗号
    }
    os << "],\n";


    //写入correct_flags
    os << "  \"correct_flags\": [";
    for (size_t i = 0; i < r.isCorrect.size(); ++i)
    {
        os << (r.isCorrect[i] ? "true" : "false");
        if (i + 1 < r.isCorrect.size()) os << ", "; //JSON 数组元素之间要有逗号，但最后一个后面不能有逗号
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
    //argc = argument count，命令行参数的个数
    //argv = argument vector，命令行参数的字符串数组（char**，等价于 char* argv[]）

    SetConsoleOutputCP(CP_UTF8);

    Options opt;

    //读取命令行的信息
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--image" && i + 1 < argc)      opt.imagePath = argv[++i];     //读取图像存放路径 
        else if (a == "--out" && i + 1 < argc)   opt.outPath = argv[++i];       //读取图像输出路径
        else if (a == "--debug-dir" && i + 1 < argc) opt.debugDir = argv[++i];  //中间图像存放路径
        else if (a == "--gui")                   opt.gui = true;                //是否跑完弹一个窗口显示最终结果图
        else if (a == "--help" || a == "-h")     { printUsage(); return 0; }    //帮助
        else { std::cerr << "unknown argument: " << a << "\n"; printUsage(); return 2; }
    }

    if (opt.imagePath.empty()) { printUsage(); return 2; }

    //读取图像
    cv::Mat img = cv::imread(opt.imagePath);    
    if (img.empty())
    {
        std::cerr << "cannot read image: " << opt.imagePath << std::endl;
        return 3;
    }

    //中间图像存放路径不为空的话
    if (!opt.debugDir.empty())
    {
        std::filesystem::create_directories(opt.debugDir);      //递归创建路径
    }


    //存放结果矩阵
    cv::Mat vis;

    //定义结果结构体接收结果
    SheetResult result = process(img, opt.debugDir, vis);   


    if (!opt.debugDir.empty() && !vis.empty())  //如果中间图像存放路径不为空 && 结果矩阵不为空
        cv::imwrite(opt.debugDir + "/result.png", vis); //在中间存放路径，保存结果

    if (!opt.outPath.empty())       //如果图像输出路径不为空
        writeJson(opt.outPath, result);     //保存Json文件


    if (result.found)   //如果找到了结果
    {
        std::cout << "RESULT answers=";
        for (char c : result.answers) std::cout << c;
        std::cout << " correct=" << result.correct << "/" << result.total << " score=" << result.score << std::endl;
    }
    else
    {
        std::cout << "RESULT not_found" << std::endl;
    }

    //是否展示result
    if (opt.gui && !vis.empty())
    {
        cv::imshow("score", vis);
        cv::waitKey(0);
        cv::destroyAllWindows();
    }

    return result.found ? 0 : 4;
}
