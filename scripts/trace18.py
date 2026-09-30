#!/usr/bin/env python3
import io
p = '/root/csgo-src/CSGO-Source-Linux-20260928/android/native/android_main.cpp'
s = io.open(p, encoding='utf-8', errors='replace').read()

# 1) manifest 释放：LoadLocalFile 用 new[] 分配，配 delete[]（SDL_free 不匹配）
s = s.replace('''    std::istringstream manifest(std::string(static_cast<const char *>(manifestData), manifestLength));
    SDL_free(manifestData);''',
'''    std::istringstream manifest(std::string(static_cast<const char *>(manifestData), manifestLength));
    delete[] static_cast<char *>(manifestData);''')

# 2) 循环内 asset 读取换 LoadLocalFile
s = s.replace('''        const std::string asset = (uiAssets / name).string();
        size_t length = 0;
        void *data = SDL_LoadFile(asset.c_str(), &length);
        if (!data) throw std::runtime_error("Missing packaged UI asset: " + asset);''',
'''        const std::string asset = (uiAssets / name).string();
        size_t length = 0;
        void *data = LoadLocalFile(asset, &length);
        if (!data) throw std::runtime_error("Missing packaged UI asset: " + asset);''')

# 3) SDL_free(data) -> delete[]
s = s.replace('''        if (error) { SDL_free(data); throw std::runtime_error("Cannot create mobile UI asset directory"); }''',
'''        if (error) { delete[] static_cast<char *>(data); throw std::runtime_error("Cannot create mobile UI asset directory"); }''')
s = s.replace('''        if (file && fclose(file) != 0) saved = false;
        SDL_free(data);''',
'''        if (file && fclose(file) != 0) saved = false;
        delete[] static_cast<char *>(data);''')

io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('converted, remaining SDL_LoadFile:', s.count('SDL_LoadFile'))
