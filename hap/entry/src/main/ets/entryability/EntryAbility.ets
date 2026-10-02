import { abilityAccessCtrl, AbilityConstant, UIAbility, Want } from '@kit.AbilityKit'
import { hilog } from '@kit.PerformanceAnalysisKit'
import { i18n } from '@kit.LocalizationKit'
import { inputMethod } from '@kit.IMEKit'
import pointer from '@ohos.multimodalInput.pointer'
import common from '@ohos.app.ability.common'
import window from '@ohos.window'
import { resourceManager } from '@kit.LocalizationKit'
import fs from '@ohos.file.fs'
import sdl from 'libSDL3.so'
import entry from 'libentry.so'

const DOMAIN = 0x4353

export default class EntryAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    hilog.info(DOMAIN, 'CSGOHOS', '%{public}s', 'Ability onCreate')
    // UIAbility 里必须用 this.context；全局 getContext(this) 在此阶段返回 undefined
    const context = this.context
    try {
      sdl.provideArkTSObjects(this, abilityAccessCtrl.createAtManager(), i18n.System, pointer.setPointerVisibleSync, inputMethod.getController())
      hilog.info(DOMAIN, 'CSGOHOS', 'provideArkTSObjects ok')
    } catch (e) {
      hilog.error(DOMAIN, 'CSGOHOS', 'provideArkTSObjects failed: %{public}s', String(e))
    }
    // 联调命令注入文件必须在 setGameRoot 之前落盘（napi SetGameRoot 读它写进
    // CSGO_OHOS_ARGS——时序曾反了导致注入进不了引擎命令行）
    try {
      const cmd: Uint8Array = context.resourceManager.getRawFileContentSync('cmdline.txt')
      const f = fs.openSync(context.filesDir + '/cmdline.txt',
        fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC)
      if (cmd.length > 0) {
        fs.writeSync(f.fd, cmd.buffer as object as ArrayBuffer)
      }
      fs.closeSync(f)
      hilog.info(DOMAIN, 'CSGOHOS', 'cmdline.txt seeded: %{public}s',
        String.fromCharCode(...Array.from(cmd.subarray(0, Math.min(cmd.length, 120)))))
    } catch (e) {
      // 无 cmdline.txt（正常发布包）——静默
    }
    try {
      entry.setGameRoot(context.filesDir + '/csgo', context.filesDir)
      entry.prepareGameEnv()
      hilog.info(DOMAIN, 'CSGOHOS', 'game env prepared at %{public}s', context.filesDir + '/csgo')
    } catch (e) {
      hilog.error(DOMAIN, 'CSGOHOS', 'entry setup failed: %{public}s', String(e))
    }
    // 同步解包（onCreate 内一次跑完）：SDL3 在 XComponent surface 创建后立即
    // dlopen libmain.so 并启动引擎，异步解包会和引擎抢资源目录（曾出现引擎先于
    // 解包读 mobile_ui 的竞态）。种子量小（~90 个小文件），同步 IO 开销可接受。
    try {
      this.extractGameSeed(context, 'csgo', context.filesDir + '/csgo')
    } catch (e) {
      hilog.error(DOMAIN, 'CSGOHOS', 'seed extraction failed: %{public}s', String(e))
    }
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    // 沉浸式全屏：内容延伸进导航条区域，去掉底部白条；启动页/游戏都全屏
    const mainWindow = windowStage.getMainWindowSync()
    mainWindow.setWindowLayoutFullScreen(true)
    mainWindow.setWindowSystemBarProperties({
      navigationBarColor: '#00000000',
      statusBarColor: '#00000000'
    })
    // 联调期永久不息屏：引擎长时间加载与游戏运行都不允许系统灭屏
    mainWindow.setWindowKeepScreenOn(true)
    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        hilog.error(DOMAIN, 'CSGOHOS', 'loadContent failed: %{public}s', JSON.stringify(err) ?? '')
      }
    })
  }

  /**
   * 递归解包 rawfile 目录到沙箱（全同步）。文件/目录判定：
   * getRawFileContentSync 成功即文件（含零长度文件），抛错即目录并递归。
   */
  private extractGameSeed(context: common.UIAbilityContext, rawDir: string, destDir: string): void {
    const rm: resourceManager.ResourceManager = context.resourceManager
    try {
      fs.mkdirSync(destDir, true)
    } catch (e) {
      // 目录已存在
    }
    let names: Array<string> = []
    try {
      names = rm.getRawFileListSync(rawDir + '/')
    } catch (e) {
      hilog.error(DOMAIN, 'CSGOHOS', 'rawfile list failed: %{public}s', rawDir)
      return
    }
    for (const name of names) {
      const rawPath = rawDir + '/' + name
      const destPath = destDir + '/' + name
      let content: Uint8Array
      try {
        content = rm.getRawFileContentSync(rawPath)
      } catch (e) {
        // 目录：递归
        this.extractGameSeed(context, rawPath, destPath)
        continue
      }
      try {
        const file = fs.openSync(destPath, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC)
        if (content.length > 0) {
          fs.writeSync(file.fd, content.buffer as object as ArrayBuffer)
        }
        fs.closeSync(file)
      } catch (e) {
        hilog.error(DOMAIN, 'CSGOHOS', 'write failed: %{public}s', destPath)
      }
    }
    hilog.info(DOMAIN, 'CSGOHOS', 'seed extracted: %{public}s (%{public}d entries)', rawDir, names.length)
  }
}