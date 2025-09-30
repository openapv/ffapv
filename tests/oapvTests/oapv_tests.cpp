#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <string>
#include <iostream>

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
        // Generate test video source
        std::string genCmd =
            "ffmpeg -y -f lavfi -i testsrc=size=" + GetParam().resolution +
            ":rate=25 -t 2 -pix_fmt yuv422p10le " + inputFile + " > /dev/null 2>&1";
        std::system(genCmd.c_str());
    }

    void TearDown() override {
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
        " -c:v " + p.codec + " " +encodedFile +
        " > /dev/null 2>&1";

    long long encTime = RunCommandAndMeasure(encodeCmd);
    std::cout << "[" << p.name << "] Encoding took " << encTime << " ms\n";

    // Record encode timing into gtest XML
    RecordProperty("encode_time_ms", encTime);

    // Decode
    std::string decodeCmd =
        "ffmpeg -y -i " + encodedFile +
        " -f rawvideo -pix_fmt yuv422p10le " + decodedFile +
        " > /dev/null 2>&1";

    long long decTime = RunCommandAndMeasure(decodeCmd);
    std::cout << "[" << p.name << "] Decoding took " << decTime << " ms\n";

    // Record decode timing into gtest XML
    RecordProperty("decode_time_ms", decTime);

    // Optional thresholds
    EXPECT_LT(encTime, 5000);
    EXPECT_LT(decTime, 5000);
}

// Add here more tests combinations!
INSTANTIATE_TEST_SUITE_P(
    FFmpegBenchmarks,
    FFmpegCmdPerfTest,
    ::testing::Values(
        FFmpegParams{"liboapv", "320x240", "oapv_240p"},
        FFmpegParams{"liboapv", "1280x720", "oapv_720p"},
        FFmpegParams{"liboapv", "1920x1080", "oapv_1080p"},
        FFmpegParams{"liboapv", "4096x2160", "oapv_4k"},
        FFmpegParams{"liboapv", "7680x4320", "oapv_8k"}
    ),
    [](const ::testing::TestParamInfo<FFmpegCmdPerfTest::ParamType>& info) {
        return info.param.name; // readable test case name
    }
);