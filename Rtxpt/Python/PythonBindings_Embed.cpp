/*
* Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

// Embed-mode entry point: registers the `rtxpt` module that the embedded
// CPython interpreter (running inside Rtxpt.exe) will see when scripts run
// `import rtxpt`.

#if RTXPT_WITH_PYTHON

#include "PythonBindingsCore.h"

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include "../Sample.h"
#include "../SampleUI.h"

#include <stdexcept>

namespace nb = nanobind;

namespace
{
    Sample& RequireSample()
    {
        if (!g_pythonSampleSingleton)
            throw std::runtime_error("rtxpt: no Sample instance bound (renderer not initialized?)");
        return *g_pythonSampleSingleton;
    }
}

NB_MODULE(rtxpt, m)
{
    m.doc() = "RTXPT embedded Python bindings (in-process scripting host).";

    rtxpt_py::RegisterCoreBindings(m);

    m.def("app", []() -> Sample* { return &RequireSample(); },
          nb::rv_policy::reference,
          "Return the singleton Sample renderer running in this Rtxpt.exe.");

    m.def("settings", []() -> SampleUIData* { return &g_sampleUIData; },
          nb::rv_policy::reference,
          "Shortcut for rtxpt.app().settings.");

    m.attr("MODE") = "embed";
}

#endif // RTXPT_WITH_PYTHON
