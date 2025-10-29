#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <string>
#include <iostream>
#include <cstdio>
#include <filesystem>
#include <regex>

// Performance thresholds in milliseconds
constexpr long long ENCODING_TIME_THRESHOLD_MS = 5000;
constexpr long long DECODING_TIME_THRESHOLD_MS = 5000;
constexpr long long OAPV_DECODING_TIME_THRESHOLD_MS = 5000;

struct FFmpegParams {
    std::string codec;
    std::string resolution;
    std::string name;
};

class FFmpegCmdPerfTest : public ::testing::TestWithParam<FFmpegParams> {
protected:
    std::string inputFile = "test_input.yuv";
    std::string encodedFile = "test_output.mp4";
    std::string decodedFile = "test_decoded.yuv";

    void SetUp() override {
        // Validate resolution format
        std::regex resolution_pattern(R"(^\d+x\d+$)");
        ASSERT_TRUE(std::regex_match(GetParam().resolution, resolution_pattern)) 
            << "Invalid resolution format: " << GetParam().resolution;
        
        // Check if ffmpeg is available
        int ffmpeg_check = std::system("ffmpeg -version > /dev/null 2>&1");
        ASSERT_EQ(ffmpeg_check, 0) << "ffmpeg is not available in PATH";
        
        // Generate test video source
        std::string genCmd =
            "ffmpeg -y -f lavfi -i testsrc=size=" + GetParam().resolution +
            ":rate=25 -t 2 -pix_fmt yuv422p10le " + inputFile + " > /dev/null 2>&1";
        int ret = std::system(genCmd.c_str());
        ASSERT_EQ(ret, 0) << "Failed to generate test input file: " << genCmd;
    }

    void TearDown() override {
        // Clean up temporary files
        std::remove(encodedFile.c_str());
        std::remove(decodedFile.c_str());
        std::remove(inputFile.c_str());
    }

    long long RunCommandAndMeasure(const std::string& cmd) {
        auto start = std::chrono::high_resolution_clock::now();
        int ret = std::system(cmd.c_str());
        auto end = std::chrono::high_resolution_clock::now();
        EXPECT_EQ(ret, 0) << "Command failed: " << cmd;
        return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    }
};

TEST_P(FFmpegCmdPerfTest, EncodeDecode) {
    auto p = GetParam();

    // Encode
    std::string encodeCmd =
        "ffmpeg -y -f rawvideo -pix_fmt yuv422p10le -s:v " + p.resolution +
        " -i " + inputFile +
        " -c:v " + p.codec + " " + encodedFile +
        " > /dev/null 2>&1";

    long long encTime = RunCommandAndMeasure(encodeCmd);
    std::cout << "[" << p.name << "] Encoding took " << encTime << " ms\n";

    // Record encode timing into gtest XML
    RecordProperty("encode_time_ms", encTime);

    // Decode native
    std::string decodeCmd =
        "ffmpeg -y -i " + encodedFile +
        " -f rawvideo -pix_fmt yuv422p10le " + decodedFile +
        " > /dev/null 2>&1";

    long long decTime = RunCommandAndMeasure(decodeCmd);
    std::cout << "[" << p.name << "] Decoding took " << decTime << " ms\n";

    // Record decode timing into gtest XML
    RecordProperty("Native_decode_time_ms", decTime);

    // Decode oapv
    std::string decodeOapvCmd =
        "ffmpeg -y -c:v liboapv -i " + encodedFile +
        " -f rawvideo -pix_fmt yuv422p10le " + decodedFile +
        " > /dev/null 2>&1";

    long long decOapvTime = RunCommandAndMeasure(decodeOapvCmd);
    std::cout << "[" << p.name << "] OAPV decoding took " << decOapvTime << " ms\n";

    // Record decode timing into gtest XML
    RecordProperty("OAPV_decode_time_ms", decOapvTime);

    // Optional thresholds
    EXPECT_LT(encTime, ENCODING_TIME_THRESHOLD_MS);
    EXPECT_LT(decTime, DECODING_TIME_THRESHOLD_MS);
    EXPECT_LT(decOapvTime, OAPV_DECODING_TIME_THRESHOLD_MS);
}

// Add here more tests combinations!
INSTANTIATE_TEST_SUITE_P(
    FFmpegBenchmarks,
    FFmpegCmdPerfTest,
    ::testing::Values(
        FFmpegParams{"liboapv", "320x240", "apv_240p"},
        FFmpegParams{"liboapv", "1280x720", "apv_720p"},
        FFmpegParams{"liboapv", "1920x1080", "apv_1080p"},
        FFmpegParams{"liboapv", "4096x2160", "apv_4k"},
        FFmpegParams{"liboapv", "7680x4320", "apv_8k"}
    ),
    [](const ::testing::TestParamInfo<FFmpegCmdPerfTest::ParamType>& info) {
        return info.param.name; // readable test case name
    }
);
