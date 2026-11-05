// Backend Vulkan (volk charge vulkan-1 dynamiquement)
#pragma once
#include <volk.h>
#include <SDL3/SDL.h>

bool VulkanInit(SDL_Window* window);
void VulkanPresent(float r, float g, float b);
