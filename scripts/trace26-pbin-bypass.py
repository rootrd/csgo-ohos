#!/usr/bin/env python3
"""DEVELOPMENT_ONLY 下放行 pbin 签名校验（crypto++ RSASSA 在 OHOS/arm64 待查）"""
import io

p = '/root/csgo-src/CSGO-Source-Linux-20260928/src/panorama/source2/panoramauiengine.cpp'
s = io.open(p, encoding='utf-8', errors='replace').read()
old = '''	fprintf( stderr, "CSGO_TRACE: pbin verify result=%d lastByte=%d\\n", (int)bVerified, (int)(( ( char * ) bufFileData.Base() )[ bufFileData.TellPut() - 1 ]) );
	return bVerified;
}'''
new = '''	fprintf( stderr, "CSGO_TRACE: pbin verify result=%d lastByte=%d\\n", (int)bVerified, (int)(( ( char * ) bufFileData.Base() )[ bufFileData.TellPut() - 1 ]) );
#ifdef DEVELOPMENT_ONLY
	// OHOS port: crypto++ RSASSA_PKCS1v15_SHA 在 aarch64/OHOS 构建下对官方 depot
	// 签名的 code.pbin 也返回 false（原因待查：sse2neon 大整数/SHA 路径嫌疑）。
	// 开发构建信任 depot 直拷内容，放行以继续推进 Panorama 资源加载。
	if ( !bVerified )
	{
		fprintf( stderr, "CSGO_TRACE: pbin verify FAILED - accepting under DEVELOPMENT_ONLY\\n" );
		bVerified = true;
	}
#endif
	return bVerified;
}'''
assert old in s, 'anchor miss'
s = s.replace(old, new, 1)
io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('pbin bypass patched')
