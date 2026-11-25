// vrcore - placeholder for the debug-only shader overlay of the original framework.
#pragma once

#include <memory>

#include "Mod.hpp"

class ShaderDebugOverlay : public Mod {
public:
    static std::shared_ptr<ShaderDebugOverlay>& Get() {
        static auto inst = std::make_shared<ShaderDebugOverlay>();
        return inst;
    }

    [[nodiscard]] std::string_view get_name() const override { return "ShaderDebugOverlay"; }
};
