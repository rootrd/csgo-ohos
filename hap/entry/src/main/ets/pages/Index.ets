import { hilog } from '@kit.PerformanceAnalysisKit'
import promptAction from '@ohos.promptAction'
import common from '@ohos.app.ability.common'
import { ResManager, isResourceComplete } from './ResourceManager'

const DOMAIN = 0x4353

enum PageState {
  Launcher,
  Game
}

@Entry
@Component
struct Index {
  @State pageState: PageState = PageState.Launcher
  @State resourceReady: boolean = false
  @State busy: boolean = false
  @State progressFrac: number = 0
  @State progressText: string = ''
  @State downloadUrl: string = ''
  @State showUrlInput: boolean = false
  private resManager: ResManager | null = null

  aboutToAppear(): void {
    const context = getContext(this) as common.UIAbilityContext
    this.resManager = new ResManager(context, context.filesDir + '/csgo')
    this.resManager.onProgress = (frac: number, text: string) => {
      this.progressFrac = frac
      this.progressText = text
    }
    this.checkResource()
  }

  private checkResource(): void {
    const context = getContext(this) as common.UIAbilityContext
    this.resourceReady = isResourceComplete(context.filesDir + '/csgo')
  }

  private async doImport(): Promise<void> {
    if (this.busy || !this.resManager) {
      return
    }
    this.busy = true
    try {
      const ok = await this.resManager.importLocalZip()
      if (ok) {
        promptAction.showToast({ message: '资源导入完成' })
      }
    } catch (e) {
      promptAction.showToast({ message: '导入失败: ' + String(e) })
    }
    this.busy = false
    this.checkResource()
  }

  private async doDownload(): Promise<void> {
    if (this.busy || !this.resManager) {
      return
    }
    if (!this.downloadUrl || this.downloadUrl.indexOf('http') !== 0) {
      promptAction.showToast({ message: '请先填写资源包 URL' })
      return
    }
    this.busy = true
    try {
      await this.resManager.downloadAndInstall(this.downloadUrl)
      promptAction.showToast({ message: '下载安装完成' })
    } catch (e) {
      promptAction.showToast({ message: '下载失败: ' + String(e) })
    }
    this.busy = false
    this.checkResource()
  }

  private startGame(): void {
    hilog.info(DOMAIN, 'CSGOHOS', 'launcher: starting game')
    this.pageState = PageState.Game // XComponent 挂载 → SDL3 dlopen libmain.so → 引擎启动
  }

  build() {
    Stack() {
      if (this.pageState === PageState.Game) {
        // 引擎页：SDL3 接管渲染与输入
        XComponent({ id: 'csgoXComponent', type: XComponentType.SURFACE, libraryname: 'SDL3' })
          .id('csgoXComponent')
          .width('100%')
          .height('100%')
          .onLoad(() => {
            hilog.info(DOMAIN, 'CSGOHOS', 'XComponent loaded')
          })
      } else {
        // 启动页：资源管理 + 启动入口
        Column({ space: 16 }) {
          Text('CS:GO')
            .fontSize(48)
            .fontWeight(FontWeight.Bold)
            .fontColor('#F2981F')
            .margin({ top: 64 })
          Text('Counter-Strike: Global Offensive · 鸿蒙版')
            .fontSize(14)
            .fontColor('#AAAAAA')

          // 资源状态
          Row({ space: 8 }) {
            Circle({ width: 10, height: 10 })
              .fill(this.resourceReady ? '#4CC94C' : '#E0B94A')
            Text(this.resourceReady ? '游戏资源已就绪' : '缺少完整游戏资源（code.pbin / pak01）')
              .fontSize(14)
              .fontColor(this.resourceReady ? '#4CC94C' : '#E0B94A')
          }
          .margin({ top: 18 })

          // 进度条（下载/导入时显示）
          if (this.busy) {
            Column({ space: 6 }) {
              Progress({ value: Math.floor(this.progressFrac * 100), total: 100, type: ProgressType.Linear })
                .width('86%')
                .color('#F2981F')
              Text(this.progressText)
                .fontSize(12)
                .fontColor('#888888')
            }
          }

          // 启动按钮
          Button('启动游戏', { type: ButtonType.Capsule })
            .width('72%')
            .height(48)
            .fontSize(18)
            .margin({ top: 22 })
            .backgroundColor(this.resourceReady && !this.busy ? '#F2981F' : '#555555')
            .enabled(this.resourceReady && !this.busy)
            .onClick(() => this.startGame())

          Row({ space: 14 }) {
            Button('本地导入资源包', { type: ButtonType.Capsule })
              .width('34%')
              .height(40)
              .fontSize(14)
              .backgroundColor('#3A3A3A')
              .enabled(!this.busy)
              .onClick(() => this.doImport())
            Button('在线下载', { type: ButtonType.Capsule })
              .width('34%')
              .height(40)
              .fontSize(14)
              .backgroundColor('#3A3A3A')
              .enabled(!this.busy)
              .onClick(() => {
                this.showUrlInput = !this.showUrlInput
              })
          }

          if (this.showUrlInput) {
            Column({ space: 8 }) {
              TextInput({ text: this.downloadUrl, placeholder: '资源包 zip 的直链 URL（自建服务器）' })
                .width('86%')
                .height(40)
                .fontSize(13)
                .onChange((value: string) => {
                  this.downloadUrl = value
                })
              Button('开始下载', { type: ButtonType.Capsule })
                .width('50%')
                .height(38)
                .fontSize(14)
                .backgroundColor('#F2981F')
                .enabled(!this.busy)
                .onClick(() => this.doDownload())
              Text('提示：需要 zip 直链（内容为 csgo/ + platform/ 或其父目录）。Steam depot 协议下载开发中。')
                .fontSize(11)
                .fontColor('#777777')
                .width('86%')
            }
            .margin({ top: 8 })
          }
        }
        .width('100%')
        .height('100%')
        .backgroundColor('#141414')
        .alignItems(HorizontalAlign.Center)
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#000000')
  }
}
