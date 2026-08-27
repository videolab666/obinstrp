from pathlib import Path


def replace_once(path, old, new, label):
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    if new in text:
        return
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected one match, found {count}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


# 1) Expose the active OBS adapter vendor and conservative capability gates.
replace_once(
    "src/sr-gpu-video.h",
    "bool sr_gpu_replay_zero_copy_available(void);\n",
    """bool sr_gpu_replay_zero_copy_available(void);\n\n#define SR_GPU_VENDOR_ID_INTEL 0x8086u\n#define SR_GPU_VENDOR_ID_NVIDIA 0x10DEu\n#define SR_GPU_VENDOR_ID_AMD 0x1002u\n\n/* PCI vendor id of the D3D11 adapter that owns OBS's compositor device.\n * Returns 0 when the active renderer is not Windows/D3D11 or the adapter\n * cannot be resolved. */\nuint32_t sr_gpu_active_adapter_vendor_id(void);\n\n/* PROGRAM texture encoding is zero-copy only when the encoder and the OBS\n * compositor live on the same D3D11 adapter. The current GPU encoder backend\n * implements NVENC on NVIDIA and AMF on AMD; Intel/QSV texture interop is not\n * implemented yet. */\nbool sr_gpu_program_texture_encode_available(void);\n\n/* Multiview can open several independent replay decoders. Intel hybrid/iGPU\n * drivers have shown whole-OBS stalls when several FFmpeg D3D11VA decoders\n * share OBS's immediate/video context. Keep A/B replay on the native path,\n * but use software decode for multiview on Intel and unknown adapters. */\nbool sr_gpu_multiview_hardware_decode_safe(void);\n""",
    "gpu capability declarations",
)

replace_once(
    "src/sr-gpu-video.cpp",
    "#include <d3d11.h>\n",
    "#include <d3d11.h>\n#include <dxgi.h>\n",
    "dxgi include",
)

replace_once(
    "src/sr-gpu-video.cpp",
    """template<typename T> static void com_release(T *&value)\n{\n\tif (value) {\n\t\tvalue->Release();\n\t\tvalue = nullptr;\n\t}\n}\n""",
    """template<typename T> static void com_release(T *&value)\n{\n\tif (value) {\n\t\tvalue->Release();\n\t\tvalue = nullptr;\n\t}\n}\n\nstatic uint32_t d3d11_device_vendor_id(ID3D11Device *device)\n{\n\tif (!device)\n\t\treturn 0;\n\n\tIDXGIDevice *dxgi_device = nullptr;\n\tif (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi_device))))\n\t\treturn 0;\n\n\tIDXGIAdapter *adapter = nullptr;\n\tuint32_t vendor = 0;\n\tif (SUCCEEDED(dxgi_device->GetAdapter(&adapter)) && adapter) {\n\t\tDXGI_ADAPTER_DESC desc = {};\n\t\tif (SUCCEEDED(adapter->GetDesc(&desc)))\n\t\t\tvendor = desc.VendorId;\n\t}\n\tcom_release(adapter);\n\tcom_release(dxgi_device);\n\treturn vendor;\n}\n""",
    "d3d11 vendor helper",
)

replace_once(
    "src/sr-gpu-video.cpp",
    """extern \"C\" bool sr_gpu_replay_zero_copy_available(void)\n{\n#ifdef _WIN32\n\tbool available = false;\n\tobs_enter_graphics();\n\tavailable = gs_get_device_type() == GS_DEVICE_DIRECT3D_11 && gs_get_device_obj() != nullptr;\n\tobs_leave_graphics();\n\treturn available;\n#else\n\treturn false;\n#endif\n}\n""",
    """extern \"C\" bool sr_gpu_replay_zero_copy_available(void)\n{\n#ifdef _WIN32\n\tbool available = false;\n\tobs_enter_graphics();\n\tavailable = gs_get_device_type() == GS_DEVICE_DIRECT3D_11 && gs_get_device_obj() != nullptr;\n\tobs_leave_graphics();\n\treturn available;\n#else\n\treturn false;\n#endif\n}\n\nextern \"C\" uint32_t sr_gpu_active_adapter_vendor_id(void)\n{\n#ifdef _WIN32\n\tuint32_t vendor = 0;\n\tobs_enter_graphics();\n\tif (gs_get_device_type() == GS_DEVICE_DIRECT3D_11)\n\t\tvendor = d3d11_device_vendor_id(static_cast<ID3D11Device *>(gs_get_device_obj()));\n\tobs_leave_graphics();\n\treturn vendor;\n#else\n\treturn 0;\n#endif\n}\n\nextern \"C\" bool sr_gpu_program_texture_encode_available(void)\n{\n\tconst uint32_t vendor = sr_gpu_active_adapter_vendor_id();\n\treturn vendor == SR_GPU_VENDOR_ID_NVIDIA || vendor == SR_GPU_VENDOR_ID_AMD;\n}\n\nextern \"C\" bool sr_gpu_multiview_hardware_decode_safe(void)\n{\n\tconst uint32_t vendor = sr_gpu_active_adapter_vendor_id();\n\treturn vendor == SR_GPU_VENDOR_ID_NVIDIA || vendor == SR_GPU_VENDOR_ID_AMD;\n}\n""",
    "gpu capability implementations",
)

# 2) Never try to feed an Intel-owned OBS D3D11 texture/device to NVENC/AMF.
replace_once(
    "src/sr-gpu-encoder.cpp",
    """\tconst char *names[2] = {nullptr, nullptr};\n\tsize_t count = 0;\n\tswitch (backend) {\n\tcase SR_ENC_AUTO:\n\t\tnames[count++] = \"h264_nvenc\";\n\t\tnames[count++] = \"h264_amf\";\n\t\tbreak;\n\tcase SR_ENC_NVENC:\n\t\tnames[count++] = \"h264_nvenc\";\n\t\tbreak;\n\tcase SR_ENC_AMF:\n\t\tnames[count++] = \"h264_amf\";\n\t\tbreak;\n\tcase SR_ENC_QSV:\n\tcase SR_ENC_X264:\n\t\treturn nullptr;\n\t}\n""",
    """\tconst uint32_t vendor = sr_gpu_active_adapter_vendor_id();\n\tconst char *names[2] = {nullptr, nullptr};\n\tsize_t count = 0;\n\tswitch (backend) {\n\tcase SR_ENC_AUTO:\n\t\tif (vendor == SR_GPU_VENDOR_ID_NVIDIA)\n\t\t\tnames[count++] = \"h264_nvenc\";\n\t\telse if (vendor == SR_GPU_VENDOR_ID_AMD)\n\t\t\tnames[count++] = \"h264_amf\";\n\t\telse\n\t\t\treturn nullptr;\n\t\tbreak;\n\tcase SR_ENC_NVENC:\n\t\tif (vendor != SR_GPU_VENDOR_ID_NVIDIA)\n\t\t\treturn nullptr;\n\t\tnames[count++] = \"h264_nvenc\";\n\t\tbreak;\n\tcase SR_ENC_AMF:\n\t\tif (vendor != SR_GPU_VENDOR_ID_AMD)\n\t\t\treturn nullptr;\n\t\tnames[count++] = \"h264_amf\";\n\t\tbreak;\n\tcase SR_ENC_QSV:\n\tcase SR_ENC_X264:\n\t\treturn nullptr;\n\t}\n""",
    "encoder adapter gate",
)

# 3) PROGRAM should be unavailable, not FAILED, when OBS itself is on Intel.
replace_once(
    "src/sr-program-recorder.c",
    "#include \"sr-config.h\"\n",
    "#include \"sr-config.h\"\n#include \"sr-gpu-video.h\"\n",
    "program gpu include",
)

replace_once(
    "src/sr-program-recorder.c",
    """\tbool writer_failed;\n\tbool master_audio_acquired;\n""",
    """\tbool writer_failed;\n\tbool master_audio_acquired;\n\tbool gpu_supported;\n""",
    "program capability state",
)

replace_once(
    "src/sr-program-recorder.c",
    """bool sr_program_recorder_supported(void)\n{\n#ifdef _WIN32\n\treturn true;\n#else\n\treturn false;\n#endif\n}\n""",
    """bool sr_program_recorder_supported(void)\n{\n#ifdef _WIN32\n\treturn g_program.initialized && g_program.gpu_supported;\n#else\n\treturn false;\n#endif\n}\n""",
    "program supported gate",
)

replace_once(
    "src/sr-program-recorder.c",
    """\tg_program.initialized = true;\n\n#ifdef _WIN32\n\tobs_add_main_rendered_callback(program_rendered, &g_program);\n\tg_program.callback_registered = true;\n#endif\n""",
    """\tg_program.initialized = true;\n\n#ifdef _WIN32\n\tg_program.gpu_supported = sr_gpu_program_texture_encode_available();\n\tif (g_program.gpu_supported) {\n\t\tobs_add_main_rendered_callback(program_rendered, &g_program);\n\t\tg_program.callback_registered = true;\n\t} else {\n\t\tblog(LOG_INFO,\n\t\t     \"Pitel Instant Replay: PROGRAM GPU recorder disabled on the active OBS adapter; NVENC requires OBS on NVIDIA and AMF requires OBS on AMD\");\n\t}\n#endif\n""",
    "program init capability",
)

replace_once(
    "src/sr-program-recorder.c",
    "Pitel Instant Replay: PROGRAM recorder requires Windows D3D11 with NVENC or AMF; encoder unavailable",
    "Pitel Instant Replay: PROGRAM recorder could not open the hardware encoder on the active OBS D3D11 adapter",
    "program error wording",
)

# 4) Let callers explicitly request software replay decode without changing A/B.
replace_once(
    "src/sr-disk-player.h",
    """struct sr_disk_player *sr_disk_player_create_with_cache(const char *session_dir, const char *camera_name,\n\t\t\t\t\t\t\tsize_t max_cache_bytes);\nvoid sr_disk_player_destroy(struct sr_disk_player *player);\n""",
    """struct sr_disk_player *sr_disk_player_create_with_cache(const char *session_dir, const char *camera_name,\n\t\t\t\t\t\t\tsize_t max_cache_bytes);\n\n/* Selects whether newly opened segments prefer the D3D11 replay decoder.\n * Changing this closes the current decoder and clears cached frames. A/B uses\n * the default hardware-preferred mode; multiview can opt into the safer\n * software decoder on problematic hybrid/iGPU adapters. */\nvoid sr_disk_player_set_hardware_decode(struct sr_disk_player *player, bool enabled);\nvoid sr_disk_player_destroy(struct sr_disk_player *player);\n""",
    "disk player decode mode api",
)

replace_once(
    "src/sr-disk-player.c",
    """\tuint64_t decode_requests;\n\tuint64_t cache_hits;\n\tuint64_t decoded_frames;\n};\n""",
    """\tuint64_t decode_requests;\n\tuint64_t cache_hits;\n\tuint64_t decoded_frames;\n\tbool prefer_hardware_decode;\n};\n""",
    "disk player decode mode state",
)

replace_once(
    "src/sr-disk-player.c",
    """\tp->decoder = sr_decoder_create_replay(info.codec_id, info.extradata, info.extradata_size);\n""",
    """\tp->decoder = p->prefer_hardware_decode\n\t\t\t     ? sr_decoder_create_replay(info.codec_id, info.extradata, info.extradata_size)\n\t\t\t     : sr_decoder_create(info.codec_id, info.extradata, info.extradata_size);\n""",
    "disk player decoder selection",
)

replace_once(
    "src/sr-disk-player.c",
    """\tp->current_position = -1;\n\tsr_frame_cache_init(&p->frame_cache, max_cache_bytes);\n""",
    """\tp->current_position = -1;\n\tp->prefer_hardware_decode = true;\n\tsr_frame_cache_init(&p->frame_cache, max_cache_bytes);\n""",
    "disk player default decode mode",
)

replace_once(
    "src/sr-disk-player.c",
    """void sr_disk_player_destroy(struct sr_disk_player *p)\n{\n""",
    """void sr_disk_player_set_hardware_decode(struct sr_disk_player *p, bool enabled)\n{\n\tif (!p || p->prefer_hardware_decode == enabled)\n\t\treturn;\n\tp->prefer_hardware_decode = enabled;\n\tclose_stream(p);\n\tsr_frame_cache_clear(&p->frame_cache);\n}\n\nvoid sr_disk_player_destroy(struct sr_disk_player *p)\n{\n""",
    "disk player decode mode setter",
)

# 5) Multiview keeps the same sr_gpu_renderer_draw presentation path, but on
# Intel/unknown adapters it decodes to software frames to avoid multi-decoder
# D3D11VA stalls on the shared OBS device/context.
replace_once(
    "src/sr-multiview-dock.cpp",
    """\t\t\t\tif (!session.empty() && !camera.empty())\n\t\t\t\t\tplayer = sr_disk_player_create_with_cache(session.c_str(), camera.c_str(),\n\t\t\t\t\t\t\t\t\t\t  12ULL * 1024ULL * 1024ULL);\n""",
    """\t\t\t\tif (!session.empty() && !camera.empty()) {\n\t\t\t\t\tplayer = sr_disk_player_create_with_cache(session.c_str(), camera.c_str(),\n\t\t\t\t\t\t\t\t\t\t  12ULL * 1024ULL * 1024ULL);\n\t\t\t\t\tif (player)\n\t\t\t\t\t\tsr_disk_player_set_hardware_decode(player,\n\t\t\t\t\t\t\t\t\t   sr_gpu_multiview_hardware_decode_safe());\n\t\t\t\t}\n""",
    "multiview safe decode selection",
)
