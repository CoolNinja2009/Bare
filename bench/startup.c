/* startup.c: where the time to the first frame goes, step by step, with SDL
 * alone (no Bare). Set SDL_GPU_DRIVER=direct3d12|vulkan to pick the driver.
 *
 * On the reference machine (bench/README.md) creating the GPU device is the
 * floor: D3D12 350-430 ms, Vulkan ~220 ms plus ~170 ms to claim the window. */
#include <SDL3/SDL.h>
#include <stdio.h>

static Uint64 t0;

static void mark(const char *what) {
    printf("%7.1f ms  %s\n", (double)(SDL_GetTicksNS() - t0) / 1e6, what);
    fflush(stdout);
}

int main(int argc, char **argv) {
    (void)argc, (void)argv;
    t0 = SDL_GetTicksNS();
    mark("main");
    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
    mark("SDL_Init(VIDEO)");
    SDL_Window *w = SDL_CreateWindow("startup", 1280, 720, SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    mark("SDL_CreateWindow");
    SDL_GPUDevice *d = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_SPIRV, false, NULL);
    if (!d) {
        mark(SDL_GetError());
        return 1;
    }
    mark(SDL_GetGPUDeviceDriver(d));
    SDL_ClaimWindowForGPUDevice(d, w);
    mark("SDL_ClaimWindowForGPUDevice");

    SDL_GPUCommandBuffer *c = SDL_AcquireGPUCommandBuffer(d);
    SDL_GPUTexture *swap = NULL;
    SDL_WaitAndAcquireGPUSwapchainTexture(c, w, &swap, NULL, NULL);
    if (swap) {   /* some drivers give none while the window is hidden */
        SDL_GPUColorTargetInfo ct = { .texture = swap, .load_op = SDL_GPU_LOADOP_CLEAR, .store_op = SDL_GPU_STOREOP_STORE };
        SDL_EndGPURenderPass(SDL_BeginGPURenderPass(c, &ct, 1, NULL));
    }
    SDL_SubmitGPUCommandBuffer(c);
    mark(swap ? "first frame submitted" : "first frame submitted (no swapchain texture while hidden)");
    SDL_ShowWindow(w);
    mark("SDL_ShowWindow");
    SDL_WaitForGPUIdle(d);
    mark("GPU idle");

    SDL_ReleaseWindowFromGPUDevice(d, w);
    SDL_DestroyGPUDevice(d);
    SDL_DestroyWindow(w);
    SDL_Quit();
    return 0;
}
