/*
* Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#include "AdvancedSample.h"
#include <SampleCommon/SampleBaseApp.h>

#include "SampleCommon/SplashScreen.h"

class AdvancedSample : public SampleBaseApp
{
    std::unique_ptr<Sample> CreateMainRenderPass(donut::app::DeviceManager& deviceManager, const CommandLineOptions& cmdLineOptions) override
    {
        return std::make_unique<AdvancedPathTracer>(deviceManager, cmdLineOptions);
    }
};

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
#else
int main(int __argc, const char** __argv)
#endif
{
    SplashScreen splashScreen;
    splashScreen.Start(L"loading_splash.png");

    AdvancedSample example;

    // Run the sample app
    const auto status = example.Init(__argc, __argv);
    
    splashScreen.Stop();

    if (status == SampleBaseApp::InitReturnCodes::Success)
    {
        example.RunMainLoop();

        example.End();
    }
    
    return static_cast<int>(status);
}
