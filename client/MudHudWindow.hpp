#pragma once

#include <functional>
#include <string>

#include "frame/gui/gui_window_interface.h"

namespace grpcmud::client
{

class MudHudWindow final : public frame::gui::GuiWindowInterface
{
public:
    explicit MudHudWindow(std::function<bool()> draw_callback)
        : draw_callback_(std::move(draw_callback))
    {
    }

    bool DrawCallback() override
    {
        return draw_callback_();
    }

    bool End() const override
    {
        return false;
    }

    std::string GetName() const override
    {
        return name_;
    }

    void SetName(const std::string& name) override
    {
        name_ = name;
    }

private:
    std::function<bool()> draw_callback_;
    std::string name_ = "grpcMUD HUD";
};

} // namespace grpcmud::client
