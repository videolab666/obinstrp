from pathlib import Path

p = Path('src/sr-multiview-dock.cpp')
text = p.read_text(encoding='utf-8')

# Prevent Windows.h min/max macros from breaking std::min/std::max throughout the file.
text = text.replace('#ifdef _WIN32\n#define WIN32_LEAN_AND_MEAN\n#include <Windows.h>\n#endif',
                    '#ifdef _WIN32\n#define WIN32_LEAN_AND_MEAN\n#ifndef NOMINMAX\n#define NOMINMAX\n#endif\n#include <Windows.h>\n#endif')

# Use portable C++ mutex rather than pthread APIs in this C++ Qt widget.
text = text.replace('\t\tpthread_mutex_init(&frameMutex, nullptr);\n', '')
text = text.replace('\t\tpthread_mutex_lock(&frameMutex);\n', '\t\tframeMutex.lock();\n')
text = text.replace('\t\tpthread_mutex_unlock(&frameMutex);\n', '\t\tframeMutex.unlock();\n')
text = text.replace('\t\tpthread_mutex_destroy(&frameMutex);\n', '')
text = text.replace('\tpthread_mutex_t frameMutex;\n', '\tstd::mutex frameMutex;\n')

p.write_text(text, encoding='utf-8')
