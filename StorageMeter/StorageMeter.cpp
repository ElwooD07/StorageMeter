#include <iostream>
#include <thread>
#include <filesystem>
#include <assert.h>
#include <chrono>

class StopWatch
{
public:
    // Starts implicitly when created
    StopWatch()
    {
        Start();
    }

    void Start()
    {
        m_start = std::chrono::high_resolution_clock::now();
    }

    std::chrono::nanoseconds Stop()
    {
        return std::chrono::high_resolution_clock::now() - m_start;
    }

    static std::string NanosecondsToMsString(std::chrono::nanoseconds ns)
    {
        return NanosecondsToMsString(ns.count());
    }
    static std::string NanosecondsToMsString(uint64_t ns)
    {
        return std::to_string(ns / 1000000) + " ms";
    }

private:
    std::chrono::high_resolution_clock::time_point m_start = {};
};

constexpr uint32_t g_portionSize = 104857600; // 100 MegaBytes
constexpr uint32_t g_portionsCount = 10; // g_portionSize * g_portionsCount = total maximum amount of first write
using Portion = std::vector<uint8_t>;
constexpr float g_maxTestDuration = 2.0;
constexpr uint8_t g_maxSlowTests = 2;

void InitPortionWithRandomData(Portion& portion)
{
    portion.clear();
    std::cout << "Generating random data... ";

    srand(static_cast<int>(time(nullptr)));
    portion.resize(g_portionSize);

    auto sizeOfRand = sizeof(decltype(rand()));
    for (auto i = 0; i < g_portionSize / sizeOfRand; ++i)
    {
        reinterpret_cast<decltype(rand())&>(portion[i * sizeOfRand]) = rand();
    }

    std::cout << "done." << std::endl;
}

std::chrono::nanoseconds WriteTestFile(const std::filesystem::path& path, const Portion& portion)
{
    FILE* file = nullptr;
    auto err = fopen_s(&file, path.string().c_str(), "w");
    if (err != 0 || file == nullptr)
    {
        throw std::runtime_error("Failed to create file " + path.string() + ", errno=" + std::to_string(err));
    }

    StopWatch watch;
    for (auto i = 0; i < g_portionsCount; ++i)
    {
        auto res = fwrite(portion.data(), sizeof(Portion::value_type), portion.size(), file);
        if (res != portion.size())
        {
            fclose(file);
            throw std::runtime_error("Failed to write portion " + std::to_string(i) + " to file " + path.string());
        }
    }

    auto res = watch.Stop();
    fclose(file);
    return res;
}

// Changes the portion buffer size if the test was too slow
// Returns the number of nanoseconds that the test took
std::chrono::nanoseconds TestDriveWriteFirst(const std::filesystem::path& dirPath, Portion& portion)
{
    auto firstWriteTime = WriteTestFile(dirPath / "single_thread", portion);
    float firstWriteTimeSeconds = static_cast<float>(firstWriteTime.count()) / (1000 * 1000 * 1000);
    if (firstWriteTimeSeconds > g_maxTestDuration)
    {
        auto prefferedPortionSize = g_maxTestDuration / firstWriteTimeSeconds * portion.size();
        portion.resize(static_cast<size_t>(prefferedPortionSize));
    }

    return firstWriteTime;
}

std::string GetFileNameForThread(size_t threadNumber)
{
    return "thread" + std::to_string(threadNumber + 1);
}

float CalculateSpeed(const Portion& portion, size_t threadsCount, uint64_t ns)
{
    return static_cast<float>(portion.size() * g_portionsCount * threadsCount / 1024 / 1024)
        / (static_cast<float>(ns) / 1000 / 1000 / 1000);
}

std::string FormatSpeed(float speed)
{
    if (speed > 1024.0)
    {
        return std::to_string(speed / 1024.0) + " GB/s";
    }
    return std::to_string(speed) + " MB/s";
}

std::string FormatSize(size_t size)
{
    return std::to_string(static_cast<float>(size) / 1024 / 1024) + " MB";
}

// Tests write speed with several threads. Stops testing when the
// g_maxSlowTests number of tests were slower then the previous ones.
// Returns max threads tested.
size_t TestDriveWrite(const std::filesystem::path& dirPath)
{
    Portion portion;
    InitPortionWithRandomData(portion);
    uint64_t lastWriteTime = TestDriveWriteFirst(dirPath, portion).count();
    auto lastSpeed = CalculateSpeed(portion, 1, lastWriteTime);
    std::cout << "1 thread: " << StopWatch::NanosecondsToMsString(lastWriteTime)
        << ", speed: " << FormatSpeed(lastSpeed) << ", data size per thread: "
        << FormatSize(portion.size() * g_portionsCount) << "\n-----\n";

    uint8_t slowTests = 0;
    size_t threadsCount = 2;
    auto bad = false;
    while (slowTests < g_maxSlowTests)
    {
        std::vector<std::atomic<uint64_t> > results(threadsCount);
        std::vector<std::thread> threads;
        for (size_t i = 0; i < threadsCount; ++i)
        {
            threads.push_back(std::thread([&](size_t threadNumber) {
                try
                {
                    results[threadNumber] = WriteTestFile(dirPath / GetFileNameForThread(threadNumber), portion).count();
                }
                catch (const std::exception& ex)
                {
                    std::cerr << "TestDriveWrite failed: " << ex.what() << std::endl;
                    bad = true;
                }
                }, i)
            );
        }

        uint64_t currentWriteTime = 0;
        for (size_t i = 0; i < threadsCount; ++i)
        {
            threads[i].join();
            std::cout << "thread " << i + 1 << ": " << StopWatch::NanosecondsToMsString(results[i]) << std::endl;
            currentWriteTime += results[i];
        }

        if (bad)
        {
            return threadsCount;
        }

        currentWriteTime /= threadsCount;
        auto speed = (portion.size() * g_portionsCount * threadsCount * 1000 * 1000) / currentWriteTime;
        auto currentSpeed = CalculateSpeed(portion, threadsCount, currentWriteTime);
        std::cout << "Average write time: " << StopWatch::NanosecondsToMsString(currentWriteTime)
            << ", speed: " << FormatSpeed(currentSpeed) << "\n-----\n";
        if (currentSpeed < lastSpeed)
        {
            ++slowTests;
        }
        else
        {
            slowTests = 0;
        }
        lastSpeed = currentSpeed;

        ++threadsCount;
    }

    return threadsCount - 1;
}

void TestDriveRead(const std::string& dirPath, size_t maxThreadsTested)
{
    // TODO
}

void TestDrive(const std::string& dirPath)
{
    try
    {
        auto maxThreadsTested = TestDriveWrite(dirPath);
        TestDriveRead(dirPath, maxThreadsTested);
    }
    catch (const std::exception& ex)
    {
        std::cerr << "TestDrive failed: " << ex.what();
    }
}

int main()
{
    std::cout << "Enter the disk letter you want to test: ";
    auto diskLetter = static_cast<char>(std::getchar());
    char tempFolder[] = "x:\\temp";
    tempFolder[0] = diskLetter;

    // Create temp folder
    auto tempFolderExisted = true;
    try
    {
        tempFolderExisted = std::filesystem::exists(tempFolder);
        if (!tempFolderExisted)
        {
            std::filesystem::create_directory(tempFolder);
        }
    }
    catch (const std::exception& ex)
    {
        std::cerr << "Failed to create temp folder: " << ex.what() << std::endl;
    }

    TestDrive(tempFolder);

    // Delete temp folder
    if (!tempFolderExisted)
    {
        try
        {
            std::filesystem::remove(tempFolder);
        }
        catch (const std::exception& ex)
        {
            std::cerr << "Failed to remove temp folder: " << ex.what() << std::endl;
        }
    }

    system("pause");
}
