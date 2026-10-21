// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>

// Runs a cleanup when the scope ends (early returns included).
struct ScopeExit {
    std::function<void()> cleanup;
    ~ScopeExit()
    {
        if (cleanup) cleanup();
    }
};
