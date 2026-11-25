/*
 * macOSフルスクリーン中に補助ウィンドウ（ソフトウェアキーボード/ディスク選択）を
 * 開いた際、macOSのSpace切り替えでフルスクリーン画面が
 * 一旦隠れてしまう問題への対処。
 *
 * NSWindowCollectionBehaviorFullScreenAuxiliaryを補助ウィンドウのNSWindowへ
 * 設定すると、メインウィンドウがフルスクリーンのSpace上にいる間もこの
 * 補助ウィンドウを同じSpaceに重ねて表示できる（Space切り替えが起きない）。
 * SDL3自体はcrossplatform APIのためこのCocoa固有の挙動を制御する手段を
 * 持たず、`#import <Cocoa/Cocoa.h>`によるObjective-C実装が必要。
 */
#import <Cocoa/Cocoa.h>
#include "platform/debugger_launcher.h"
#include "platform/platform_macos_aux.h"
#include "platform/platform.h"

void pal_macos_set_fullscreen_auxiliary(void *nswindow_ptr) {
    /* SDLがNSWindowの所有権を持ったまま(__unsafe_unretained)貸してくるだけ
     * なので、ここでは借用するだけでretain/releaseは行わない。ARCを有効化
     * していないビルド(既定)なので素のCスタイルキャストでよい。 */
    NSWindow *window = (NSWindow *)nswindow_ptr;
    if (!window)
        return;
    window.collectionBehavior |= NSWindowCollectionBehaviorFullScreenAuxiliary;
}

/*
 * メニューバーの File/Machine/View/Tools メニュー（ディスク選択ウィンドウ(Cmd+O)、
 * Reset、Window Size(Cmd+1/Cmd+2)、ソフトウェアキーボード等）を構築する。NSMenuItem の target/action には C の関数
 * ポインタを渡せないため、Objective-Cクラスで仲介する。ARC無効ビルドの
 * ため生成したオブジェクトを明示的にreleaseしていないが、アプリ起動時に
 * 1度だけ生成しアプリ生存中ずっと使う想定（メニュー自体もNSAppに保持され
 * 続ける）なので実害はない。 */
@interface X68KMenuHandler : NSObject <NSMenuItemValidation>
- (void)openDiskSelector:(id)sender;
- (void)openSoftwareKeyboard:(id)sender;
- (void)showAbout:(id)sender;
- (void)sendInterrupt:(id)sender;
- (void)powerSwitchOff:(id)sender;
- (void)resetMachine:(id)sender;
- (void)resetMachineForceFD0:(id)sender;
- (void)setZoom1:(id)sender;
- (void)setZoom1_5:(id)sender;
- (void)setZoom2:(id)sender;
- (void)toggleFullscreen:(id)sender;
- (void)openDataFolder:(id)sender;
- (void)openDebugger:(id)sender;
@end

@implementation X68KMenuHandler
- (void)openDiskSelector:(id)sender {
    (void)sender;
    pal_aux_menu_trigger_disk_selector();
}
/* ユーザーデータフォルダ（設定・ROM・ディスク・SRAMの置き場所）をFinderで開く。
 * ~/Library は既定で隠れているため、メニューから開けるようにする。 */
- (void)openDataFolder:(id)sender {
    (void)sender;
    const char *dir = pal_get_user_dir();
    if (!dir) {
        pal_log("データフォルダを取得できません\n");
        return;
    }
    NSString *path = [NSString stringWithUTF8String:dir];
    NSURL *url = path ? [NSURL fileURLWithPath:path isDirectory:YES] : nil;
    if (!url || ![[NSWorkspace sharedWorkspace] openURL:url])
        pal_log("データフォルダを開けません: %s\n", dir);
}
/* 外部デバッガ ix68k-debugger（tools-host/debugger/）を別プロセスで起動する。
 * 失敗理由と二重起動の通知は pal_debugger_launch() が pal_log() に出す。 */
- (void)openDebugger:(id)sender {
    (void)sender;
    (void)pal_debugger_launch();
}
- (void)openSoftwareKeyboard:(id)sender {
    (void)sender;
    pal_aux_menu_trigger_keyboard();
}
- (void)showAbout:(id)sender {
    (void)sender;
    pal_aux_menu_trigger_about();
}
- (void)sendInterrupt:(id)sender {
    (void)sender;
    pal_aux_menu_trigger_interrupt();
}
- (void)powerSwitchOff:(id)sender {
    (void)sender;
    pal_aux_menu_trigger_power_switch_off();
}
- (void)resetMachine:(id)sender {
    (void)sender;
    pal_aux_menu_trigger_reset();
}
- (void)resetMachineForceFD0:(id)sender {
    (void)sender;
    pal_aux_menu_trigger_reset_fd0();
}
- (void)setZoom1:(id)sender {
    (void)sender;
    pal_aux_menu_trigger_zoom_1();
}
- (void)setZoom1_5:(id)sender {
    (void)sender;
    pal_aux_menu_trigger_zoom_1_5();
}
- (void)setZoom2:(id)sender {
    (void)sender;
    pal_aux_menu_trigger_zoom_2();
}
- (void)toggleFullscreen:(id)sender {
    (void)sender;
    pal_aux_menu_trigger_fullscreen_toggle();
}

/* メニューが開くたびにAppKitが各項目に対して呼ぶ
 * （NSMenuItemValidationプロトコル）。
 * View の倍率項目(x1/x1.5/x2)は現在の倍率に一致する項目だけチェックマーク(state=On)を
 * 立て（倍率項目はフルスクリーン中は無効化）、Fullscreen項目は
 * フルスクリーン状態を反映する。他のメニュー項目
 * (Disk Selector/Software Keyboard/Reset等)は状態を持たないワンショット
 * トリガーのため対象外（デフォルトでYESを返すだけ）。 */
- (BOOL)validateMenuItem:(NSMenuItem *)menuItem {
    int zoom_x10 = 0;
    if (menuItem.action == @selector(setZoom1:))
        zoom_x10 = 10;
    else if (menuItem.action == @selector(setZoom1_5:))
        zoom_x10 = 15;
    else if (menuItem.action == @selector(setZoom2:))
        zoom_x10 = 20;

    if (zoom_x10 != 0) {
        menuItem.state = (zoom_x10 == pal_aux_menu_current_window_zoom_x10())
                             ? NSControlStateValueOn
                             : NSControlStateValueOff;
        /* フルスクリーン中はウィンドウサイズという概念自体が無意味なため、
         * 倍率メニューを無効化する。 */
        menuItem.enabled = !pal_is_fullscreen();
    }

    if (menuItem.action == @selector(toggleFullscreen:))
        menuItem.state = pal_is_fullscreen() ? NSControlStateValueOn : NSControlStateValueOff;

    /* 起動スクリプトが無い環境（配布物等）ではデバッガ項目を無効化する。 */
    if (menuItem.action == @selector(openDebugger:))
        return pal_debugger_available() ? YES : NO;

    return YES;
}
@end

static X68KMenuHandler *s_menu_handler = nil;

/* .app 外（開発ビルドの build/ix68k）では Info.plist/AppIcon.icns が無く Dock が
 * 汎用アイコンになるため、リポジトリ内の icns を実行時に Dock へ設定する。
 * .app 起動時は Info.plist の CFBundleIconFile が効くので何もしない。 */
void pal_macos_set_dev_dock_icon(void) {
#ifdef X68K_DEV_ICON_PATH
    NSString *bundle_path = [[NSBundle mainBundle] bundlePath];
    if ([bundle_path.pathExtension isEqualToString:@"app"])
        return;
    NSImage *icon = [[NSImage alloc] initWithContentsOfFile:@X68K_DEV_ICON_PATH];
    if (icon)
        [NSApp setApplicationIconImage:icon];
#endif
}


/* メニューバーからタイトルのトップレベルメニューを探す。無ければ after_item の
 * 直後（after_item が nil なら末尾）に新設する。SDL3 が既定で作る Window/View
 * 等が既にあればそれを流用し、項目は先頭から挿入する。 */
static NSMenu *find_or_create_menu(NSMenu *mainMenu, NSString *title, NSMenuItem *after_item,
                                   NSMenuItem **out_item) {
    for (NSMenuItem *item in [mainMenu itemArray]) {
        if ([[item title] isEqualToString:title]) {
            *out_item = item;
            return [item submenu];
        }
    }
    NSMenuItem *menuItem = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
    NSMenu *menu = [[NSMenu alloc] initWithTitle:title];
    [menuItem setSubmenu:menu];
    if (after_item)
        [mainMenu insertItem:menuItem atIndex:[mainMenu indexOfItem:after_item] + 1];
    else
        [mainMenu addItem:menuItem];
    *out_item = menuItem;
    return menu;
}

/* menu の index 番目へ項目を挿入して index を進める。key が空文字ならショートカット無し。 */
static void insert_item(NSMenu *menu, NSInteger *index, NSString *title, SEL action,
                        NSString *key, NSEventModifierFlags mask) {
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key];
    if ([key length] > 0)
        [item setKeyEquivalentModifierMask:mask];
    [item setTarget:s_menu_handler];
    [menu insertItem:item atIndex:(*index)++];
}

static void insert_separator(NSMenu *menu, NSInteger *index) {
    [menu insertItem:[NSMenuItem separatorItem] atIndex:(*index)++];
}

/* メニュー構成: File / Machine / View / Tools の4分割。
 * File: ディスク・データフォルダ、Machine: 実機相当の操作、View: 表示倍率・
 * フルスクリーン、Tools: ソフトウェアキーボード・デバッガ。 */
void pal_macos_install_open_menu_item(void) {
    if (s_menu_handler)
        return; /* 二重インストール防止 */

    NSMenu *mainMenu = [NSApp mainMenu];
    if (!mainMenu)
        return;

    s_menu_handler = [[X68KMenuHandler alloc] init];

    /* SDL3が既定で作る最小メニュー（Appメニューのみ）には無いため、
     * 見つからなければ File→Machine→View→Tools の順に新設する。 */
    NSMenuItem *fileItem = nil, *machineItem = nil, *viewItem = nil, *toolsItem = nil,
               *helpItem = nil;
    NSMenu *fileMenu = find_or_create_menu(mainMenu, @"File", nil, &fileItem);
    NSMenu *machineMenu = find_or_create_menu(mainMenu, @"Machine", fileItem, &machineItem);
    NSMenu *viewMenu = find_or_create_menu(mainMenu, @"View", machineItem, &viewItem);
    NSMenu *toolsMenu = find_or_create_menu(mainMenu, @"Tools", viewItem, &toolsItem);
    NSMenu *helpMenu = find_or_create_menu(mainMenu, @"Help", toolsItem, &helpItem);

    const NSEventModifierFlags cmd = NSEventModifierFlagCommand;
    NSInteger idx = 0;

    /* File */
    insert_item(fileMenu, &idx, @"Open Disk Selector...", @selector(openDiskSelector:), @"o", cmd);
    insert_separator(fileMenu, &idx);
    /* Open Data Folder: 設定・ROM・ディスク・SRAMの置き場所（~/Library/Application
     * Support/ix68k/）をFinderで開く。 */
    insert_item(fileMenu, &idx, @"Open Data Folder", @selector(openDataFolder:), @"", 0);

    /* Machine */
    idx = 0;
    /* Reset と Force FD0 版は @"r" を共有する。AppKitのキー等価物は修飾マスク完全一致で
     * 判定されるので、この2項目が誤って両方発火することはない。
     * Force FD0 は実機の「OPT.1キーを押しながら電源投入/リセット」の再現
     * （実処理はapp/runtime.cのapp_force_fd0_boot_begin()参照）。 */
    insert_item(machineMenu, &idx, @"Reset", @selector(resetMachine:), @"r", cmd);
    insert_item(machineMenu, &idx, @"Reset (Force FD0 Boot)", @selector(resetMachineForceFD0:),
                @"r", cmd | NSEventModifierFlagOption);
    insert_separator(machineMenu, &idx);
    /* Interrupt: Pauseキーと同じNMI要求。 */
    insert_item(machineMenu, &idx, @"Interrupt", @selector(sendInterrupt:), @"", 0);
    /* Power Switch (OFF): 前面電源スイッチOFF相当。GPIP2割り込みを経由して
     * Human68kの電源断処理が走り、$E8E00Fのシーケンスでix68kが終了する。 */
    insert_item(machineMenu, &idx, @"Power Switch (OFF)", @selector(powerSwitchOff:), @"", 0);

    /* View: ウィンドウサイズ。256x256等の正方形ドットモードでネイティブ解像度が
     * 小さくなりウィンドウが縮む問題への対処で、768x512と同程度の見た目にしたい
     * ときは x2 を選ぶ想定。SRAMには保存しない（実機にない機能）。現在値には
     * チェックマークを表示する（validateMenuItem:）。 */
    idx = 0;
    insert_item(viewMenu, &idx, @"x1 (Native)", @selector(setZoom1:), @"1", cmd);
    insert_item(viewMenu, &idx, @"x1.5", @selector(setZoom1_5:), @"", 0);
    insert_item(viewMenu, &idx, @"x2", @selector(setZoom2:), @"2", cmd);
    insert_separator(viewMenu, &idx);
    insert_item(viewMenu, &idx, @"Fullscreen", @selector(toggleFullscreen:), @"", 0);

    /* Tools */
    idx = 0;
    insert_item(toolsMenu, &idx, @"Open Software Keyboard...", @selector(openSoftwareKeyboard:),
                @"k", cmd);
    /* Open Debugger: 外部デバッガ ix68k-debugger（tools-host/debugger/）を起動する。
     * 開発ツリー向け。起動スクリプトが無い環境では項目を無効化する。 */
    insert_item(toolsMenu, &idx, @"Open Debugger...", @selector(openDebugger:), @"", 0);

    /* Help: バージョンを表示する About ダイアログ。 */
    idx = 0;
    insert_item(helpMenu, &idx, @"About ix68k...", @selector(showAbout:), @"", 0);
}
