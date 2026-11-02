// Rendu Vulkan des dessins Xenos (appelé par le processeur de commandes de gpu.cpp)
#pragma once
#include <cstdint>

struct TranslatedShader;

void RenderSetShader(bool pixel, const uint8_t* ucode, uint32_t sizeBytes, const TranslatedShader* shader);
void RenderDraw(const uint32_t* regs, uint32_t initiator, uint32_t (*phys)(uint32_t), uint32_t dmaBase = 0, uint32_t dmaSize = 0);
void RenderPresent();
