#pragma once

// Builds the ImGui window each frame. Called from inside the Present hook
// between ImGui::NewFrame() and ImGui::Render().
namespace Menu
{
    void Render();
}
