import fs from '@ohos.file.fs'
import zlib from '@ohos.zlib'
import http from '@ohos.net.http'
import { hilog } from '@kit.PerformanceAnalysisKit'
import picker from '@ohos.file.picker'
import common from '@ohos.app.ability.common'

const DOMAIN = 0x4353
const TAG = 'CSGOHOS/RES'

// 完整资源判定：Panorama 主代码包或主 VPK 存在即视为可启动
export function isResourceComplete(gameRoot: string): boolean {
  try {
    if (fs.accessSync(gameRoot + '/csgo/panorama/code.pbin')) {
      return true
    }
  } catch (e) {
  }
  try {
    if (fs.accessSync(gameRoot + '/csgo/pak01_dir.vpk')) {
      return true
    }
  } catch (e) {
  }
  return false
}

async function ensureDir(path: string): Promise<void> {
  try {
    await fs.mkdir(path, true)
  } catch (e) {
    // 已存在
  }
}

async function rmRecursive(path: string): Promise<void> {
  try {
    await fs.rmdir(path)
  } catch (e) {
  }
}

// 把解压临时目录的内容并进 gameRoot（来源目录内若有 csgo/ 顶级目录则整体并入）
async function mergeIntoGameRoot(stagingDir: string, gameRoot: string): Promise<void> {
  const entries: Array<string> = await fs.listFile(stagingDir)
  if (entries.indexOf('csgo') >= 0 || entries.indexOf('hl2') >= 0) {
    await mergeDir(stagingDir, gameRoot)
  } else {
    // 压缩包里直接是 gameinfo.txt / materials/ 等：并入 gameRoot/csgo
    await ensureDir(gameRoot + '/csgo')
    await mergeDir(stagingDir, gameRoot + '/csgo')
  }
}

async function mergeDir(srcDir: string, dstDir: string): Promise<void> {
  await ensureDir(dstDir)
  const entries: Array<string> = await fs.listFile(srcDir)
  for (const name of entries) {
    const src = srcDir + '/' + name
    const dst = dstDir + '/' + name
    const stat = await fs.stat(src)
    if (stat.isDirectory()) {
      await mergeDir(src, dst)
    } else {
      try {
        await fs.copyFile(src, dst)
      } catch (e) {
        hilog.warn(DOMAIN, TAG, 'copy failed %{public}s: %{public}s', dst, String(e))
      }
    }
  }
}

export class ResManager {
  private context: common.UIAbilityContext
  gameRoot: string
  onProgress: (frac: number, text: string) => void = () => {}

  constructor(context: common.UIAbilityContext, gameRoot: string) {
    this.context = context
    this.gameRoot = gameRoot
  }

  private report(frac: number, text: string): void {
    hilog.info(DOMAIN, TAG, '%{public}s (%{public}d%%)', text, Math.floor(frac * 100))
    this.onProgress(frac, text)
  }

  // ── 本地导入：选 zip → 拷入沙箱（异步，不阻塞主线程）→ 解压 → 并进游戏根 ──
  async importLocalZip(): Promise<boolean> {
    const picker_ = new picker.DocumentViewPicker()
    const opts = new picker.DocumentSelectOptions()
    opts.fileSuffixFilters = ['.zip']
    opts.maxSelectNumber = 1
    const uris = await picker_.select(opts)
    if (!uris || uris.length === 0) {
      return false
    }
    const srcUri = uris[0]
    this.report(0.02, '读取所选压缩包（大文件需要几分钟）')
    const srcFile = fs.openSync(srcUri, fs.OpenMode.READ_ONLY)
    const tmpZip = this.context.filesDir + '/import.zip'
    try {
      await fs.copyFile(srcFile.fd, tmpZip)
    } finally {
      fs.closeSync(srcFile)
    }
    this.report(0.15, '压缩包已就位，开始解压')

    const staging = this.context.filesDir + '/import-stage'
    await rmRecursive(staging)
    await this.decompress(tmpZip, staging)
    this.report(0.85, '合并资源到游戏目录')
    await mergeIntoGameRoot(staging, this.gameRoot)
    await rmRecursive(staging)
    try {
      fs.unlinkSync(tmpZip)
    } catch (e) {
    }
    this.report(1, '导入完成')
    return true
  }

  // ── 在线下载：GET zip → 解压（URL 可在启动页配置）──
  async downloadAndInstall(url: string): Promise<boolean> {
    this.report(0.01, '连接服务器')
    const tmpZip = this.context.filesDir + '/download.zip'
    const httpRequest = http.createHttp()
    const resp = await httpRequest.request(url, {
      method: http.RequestMethod.GET,
      expectDataType: http.HttpDataType.ARRAY_BUFFER,
      connectTimeout: 20000,
      readTimeout: 600000,
      usingCache: false
    })
    if (resp.responseCode !== 200) {
      throw new Error('HTTP ' + resp.responseCode)
    }
    const buf = resp.result as ArrayBuffer
    this.report(0.4, `下载完成 ${(buf.byteLength / 1e6).toFixed(1)} MB`)
    const file = fs.openSync(tmpZip, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC)
    try {
      fs.writeSync(file.fd, buf)
    } finally {
      fs.closeSync(file)
    }
    const staging = this.context.filesDir + '/download-stage'
    await rmRecursive(staging)
    await this.decompress(tmpZip, staging)
    this.report(0.85, '合并资源到游戏目录')
    await mergeIntoGameRoot(staging, this.gameRoot)
    await rmRecursive(staging)
    try {
      fs.unlinkSync(tmpZip)
    } catch (e) {
    }
    this.report(1, '下载安装完成')
    return true
  }

  private async decompress(zipPath: string, destDir: string): Promise<void> {
    this.report(0.5, '解压资源包（9.8GB 约需几分钟）')
    await ensureDir(destDir)
    // Promise 版（不能与回调混用；此前混用导致没等解压完就继续合并）
    await zlib.decompressFile(zipPath, destDir)
    this.report(0.8, '解压完成')
  }
}
