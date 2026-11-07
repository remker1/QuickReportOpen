// =============================================================================
// TesterTranslations.cpp - hardware tester strings (en / fr / zh).
// =============================================================================
#include "TesterTranslations.h"

// One row per key keeps en/zh/fr in sync by construction; a debug-build check
// in addTesterTranslations guards against gaps.
namespace {
struct Row { const char *key, *en, *zh, *fr; };

// The table: one row per key with its en / fr / zh text.
const Row kRows[] = {
    // --- Navigation ---
    {"ht_nav_keyboard", "Keyboard", "键盘检测", "Clavier"},
    {"ht_nav_trackpad", "Trackpad", "触摸板检测", "Pavé tactile"},
    {"ht_nav_touch", "Touchscreen", "触摸屏检测", "Écran tactile"},
    {"ht_nav_camera", "Camera", "摄像头检测", "Caméra"},
    {"ht_nav_audio", "Speaker & Mic", "扬声器和麦克风", "Haut-parleur et micro"},
    {"ht_nav_screen", "Display", "屏幕检测", "Écran"},
    {"ht_nav_cpu", "CPU", "CPU 检测", "Processeur"},
    {"ht_nav_gpu", "GPU", "GPU 检测", "Carte graphique"},
    {"ht_nav_storage", "Disk & Memory", "磁盘与内存", "Disque et mémoire"},

    // --- Common ---
    {"ht_common_coming_soon", "This module is under construction.",
     "该模块正在开发中。", "Ce module est en cours de développement."},
    {"ht_common_start", "Start", "开始", "Démarrer"},
    {"ht_common_stop", "Stop", "停止", "Arrêter"},
    {"ht_common_reset", "Reset", "重置", "Réinitialiser"},
    {"ht_common_close", "Close", "关闭", "Fermer"},
    {"ht_common_not_supported", "Not supported on this platform",
     "此平台不支持", "Non pris en charge sur cette plateforme"},
    {"ht_common_no_device", "No device detected", "未检测到设备", "Aucun périphérique détecté"},
    {"ht_common_no_permission", "Permission denied", "权限被拒绝", "Autorisation refusée"},
    {"ht_common_device_busy", "Device is in use by another application",
     "设备被其他应用占用", "Périphérique utilisé par une autre application"},
    {"ht_common_relative_note",
     "Relative score — only comparable with results from this app on the same machine.",
     "相对分数——仅适用于本应用在同一台机器上的结果对比。",
     "Score relatif — comparable uniquement aux résultats de cette application sur la même machine."},

    {"ht_common_cancel_test", "Cancel test", "取消测试", "Annuler le test"},
    {"ht_common_cancelling", "Cancelling…", "正在取消…", "Annulation…"},
    {"ht_common_cancelled", "Test cancelled.", "测试已取消。", "Test annulé."},

    // --- Keyboard ---
    {"ht_kb_tested", "Tested: {tested}/{total} keys", "已测按键:{tested}/{total}",
     "Testées : {tested}/{total} touches"},
    {"ht_kb_nkro", "Simultaneous: {now} (max {max})", "同时按下:{now}(最多 {max})",
     "Simultanées : {now} (max {max})"},
    {"ht_kb_hide_numpad", "Hide numpad", "隐藏小键盘", "Masquer le pavé numérique"},
    {"ht_kb_french_layout", "Canadian French", "加拿大法语", "Français canadien"},
    {"ht_kb_last_key", "Last key", "最近按键", "Dernière touche"},
    {"ht_kb_scancode", "Scan code", "扫描码", "Scan code"},
    {"ht_kb_vk", "Virtual key", "虚拟键码", "Touche virtuelle"},
    {"ht_kb_history", "Key history (last 12)", "按键历史(最近 12 个)",
     "Historique (12 dernières)"},
    {"ht_kb_chatter_title", "Chatter detection (<35 ms)", "连击检测(<35ms)",
     "Détection de rebond (<35 ms)"},
    {"ht_kb_chatter_none", "No chatter detected. A key re-triggering within 35 ms "
     "of its release indicates a faulty switch.",
     "未检测到连击。同一键在释放后 35ms 内重复触发说明轴体/触点故障。",
     "Aucun rebond détecté. Une touche qui se redéclenche dans les 35 ms suivant "
     "son relâchement indique un commutateur défectueux."},
    {"ht_kb_chatter_found", "{n} chatter event(s) detected:", "检测到 {n} 次连击:",
     "{n} rebond(s) détecté(s) :"},
    {"ht_kb_fn_note", "Note: the Fn key is handled by keyboard firmware and cannot "
     "be detected by software.",
     "说明:Fn 键由键盘固件处理,软件无法检测。",
     "Remarque : la touche Fn est gérée par le micrologiciel du clavier et ne peut "
     "pas être détectée par logiciel."},
    {"ht_kb_press_any", "Press any key…", "按下任意键……", "Appuyez sur une touche…"},
    {"ht_kb_result_name", "Keyboard test", "键盘检测", "Test du clavier"},

    // --- Mouse ---
    {"ht_ms_buttons_title", "Buttons", "按键检测", "Boutons"},
    {"ht_ms_buttons_hint", "Press each mouse button: left, right, middle (wheel "
     "click), and side buttons X1/X2.",
     "依次按下鼠标各键:左键、右键、中键(滚轮按下)、侧键 X1/X2。",
     "Appuyez sur chaque bouton : gauche, droit, milieu (clic molette) et boutons "
     "latéraux X1/X2."},
    {"ht_ms_dbl_title", "Double-click", "双击测试", "Double-clic"},
    {"ht_ms_dbl_hint", "Double-click anywhere to measure the interval.",
     "在任意位置双击以测量两次点击间隔。",
     "Double-cliquez n'importe où pour mesurer l'intervalle."},
    {"ht_ms_dbl_threshold", "System threshold", "系统双击阈值", "Seuil système"},
    {"ht_ms_wheel_title", "Wheel", "滚轮", "Molette"},
    {"ht_ms_poll_title", "Polling rate", "回报率", "Taux d'interrogation"},
    {"ht_ms_poll_peak", "Peak", "峰值", "Pic"},
    {"ht_ms_poll_sustained", "Sustained", "持续", "Soutenu"},
    {"ht_ms_poll_note", "Move the mouse in continuous circles to measure. Values "
     "are counted from hardware WM_INPUT events (Raw Input), so 125/500/1000 Hz "
     "and higher gaming rates are measured, not estimated.",
     "持续画圈移动鼠标进行测量。数值直接统计硬件 WM_INPUT 事件(Raw Input),"
     "125/500/1000Hz 及更高的电竞回报率都是实测而非估算。",
     "Déplacez la souris en cercles continus pour mesurer. Les valeurs proviennent "
     "des événements matériels WM_INPUT (Raw Input) : 125/500/1000 Hz et plus sont "
     "mesurés, pas estimés."},
    {"ht_ms_trail_title", "Movement trail (right-click menu disabled here)",
     "移动轨迹(此区域内右键菜单已禁用)",
     "Trajectoire (menu contextuel désactivé ici)"},
    {"ht_ms_clear_trail", "Clear trail", "清除轨迹", "Effacer la trajectoire"},
    {"ht_ms_result_name", "Mouse test", "鼠标检测", "Test de la souris"},

    // --- Trackpad ---
    {"ht_tp_device_title", "Device", "设备", "Périphérique"},
    {"ht_tp_precision_yes", "Precision touchpad detected — Windows handles its "
     "multi-finger gestures and delivers pointer, tap and scroll events to "
     "applications; those are what this page tests.",
     "已检测到高精度触摸板(Precision Touchpad)——多指手势由 Windows 处理,应用可测的是"
     "指针、点击和滚动事件,即本页内容。",
     "Pavé tactile de précision détecté — Windows gère ses gestes multi-doigts et "
     "transmet aux applications les événements de pointeur, de clic et de "
     "défilement testés ici."},
    {"ht_tp_precision_no", "No precision touchpad found (HID digitizer/touch-pad "
     "collection absent). A basic touchpad or external mouse still produces the "
     "pointer events tested below.",
     "未检测到高精度触摸板(无 HID 数字化仪/触摸板集合)。普通触摸板或外接鼠标同样会产生"
     "下方测试的指针事件。",
     "Aucun pavé tactile de précision trouvé. Un pavé basique ou une souris "
     "externe produit néanmoins les événements testés ci-dessous."},
    {"ht_tp_click_title", "Click / tap", "点击/轻触", "Clic / tape"},
    {"ht_tp_click_hint", "One-finger tap or press the lower-left zone = left "
     "click; two-finger tap or press the lower-right zone = right click.",
     "单指轻触或按压左下区域 = 左键;双指轻触或按压右下区域 = 右键。",
     "Tape à un doigt ou zone inférieure gauche = clic gauche ; tape à deux "
     "doigts ou zone inférieure droite = clic droit."},
    {"ht_tp_dbl_title", "Double-tap", "双击测试", "Double-tape"},
    {"ht_tp_dbl_hint", "Double-tap anywhere to measure the interval.",
     "在任意位置双击/双触以测量间隔。",
     "Double-tapez n'importe où pour mesurer l'intervalle."},
    {"ht_tp_scroll_title", "Two-finger scroll", "双指滚动", "Défilement à deux doigts"},
    {"ht_tp_rate_title", "Report rate", "报告率", "Taux de rapport"},
    {"ht_tp_rate_note", "Counted from hardware WM_INPUT pointer events while you "
     "move a finger continuously. Typical touchpads report at 90–140 Hz; values "
     "are measured, not estimated.",
     "持续移动手指时直接统计硬件 WM_INPUT 指针事件。常见触摸板报告率为 90–140Hz;"
     "数值为实测而非估算。",
     "Compté à partir des événements matériels WM_INPUT pendant un mouvement "
     "continu. Les pavés tactiles typiques rapportent à 90–140 Hz ; valeurs "
     "mesurées, pas estimées."},
    {"ht_tp_trail_title", "Tracking area (context menu disabled here)",
     "轨迹区域(此区域内右键菜单已禁用)",
     "Zone de suivi (menu contextuel désactivé ici)"},
    {"ht_tp_result_name", "Trackpad test", "触摸板检测", "Test du pavé tactile"},

    // --- Touchscreen ---
    {"ht_touch_usage",
     "Touch and drag across the entire canvas. Use several fingers together to verify multi-touch and check every area of the digitizer.",
     "请用手指在整个画布上拖动，并同时使用多根手指验证多点触控，同时检查触摸屏的每个区域。",
     "Touchez et faites glisser sur toute la zone. Utilisez plusieurs doigts pour vérifier le multi-touch et chaque partie du numériseur."},
    {"ht_touch_detected", "Touchscreen detected · Windows reports up to {count} contacts",
     "已检测到触摸屏 · Windows 报告最多 {count} 个触点",
     "Écran tactile détecté · Windows annonce jusqu’à {count} contacts"},
    {"ht_touch_not_detected",
     "Windows did not report a touchscreen. The canvas remains available for digitizers that expose mouse-compatible input.",
     "Windows 未报告触摸屏。画布仍可用于测试以鼠标兼容方式输出的数字化仪。",
     "Windows ne signale aucun écran tactile. La zone reste disponible pour les numériseurs émulant une souris."},
    {"ht_touch_counter", "Active touches: {active} · Maximum simultaneous: {max}",
     "当前触点：{active} · 最大同时触点：{max}",
     "Contacts actifs : {active} · Maximum simultané : {max}"},
    {"ht_touch_clear", "Clear canvas", "清除画布", "Effacer la zone"},
    {"ht_touch_fullscreen", "Full screen", "全屏测试", "Plein écran"},
    {"ht_touch_exit_fullscreen", "Exit full screen", "退出全屏", "Quitter le plein écran"},
    {"ht_touch_result_name", "Touchscreen test", "触摸屏检测", "Test de l’écran tactile"},
    {"ht_touch_result", "Touch input detected; maximum {max} simultaneous contact(s)",
     "已检测到触摸输入；最大同时触点为 {max}",
     "Entrée tactile détectée ; maximum de {max} contact(s) simultané(s)"},

    // --- Screen ---
    {"ht_scr_usage", "Solid colors reveal dead/stuck pixels, the grayscale gradient "
     "reveals banding, and black reveals backlight bleed. Values below are read "
     "directly from the OS display pipeline (DisplayConfig/EDID), not estimated.",
     "纯色用于找坏点/亮点,灰阶渐变用于查色阶断层,黑色用于查漏光。以下数值直接读自"
     "系统显示管线(DisplayConfig/EDID),非估算。",
     "Les couleurs unies révèlent les pixels morts, le dégradé de gris révèle le "
     "banding, le noir révèle les fuites de rétroéclairage. Les valeurs ci-dessous "
     "proviennent directement du pipeline d'affichage (DisplayConfig/EDID)."},
    {"ht_scr_unknown_monitor", "Unknown monitor", "未知显示器", "Moniteur inconnu"},
    {"ht_scr_resolution", "Physical resolution", "物理分辨率", "Résolution physique"},
    {"ht_scr_refresh", "Refresh rate (precise)", "刷新率(精确值)",
     "Fréquence (précise)"},
    {"ht_scr_scale", "Scale", "缩放比例", "Mise à l'échelle"},
    {"ht_scr_depth", "Color depth", "色深", "Profondeur de couleur"},
    {"ht_scr_hdr", "HDR", "HDR", "HDR"},
    {"ht_scr_hdr_on", "Supported, enabled", "支持,已开启", "Pris en charge, activé"},
    {"ht_scr_hdr_off", "Supported, disabled", "支持,未开启",
     "Pris en charge, désactivé"},
    {"ht_scr_hdr_unsupported", "Not supported", "不支持", "Non pris en charge"},
    {"ht_scr_bits_per_channel", "Bits per channel", "每通道位深", "Bits par canal"},
    {"ht_scr_edid", "Panel (EDID)", "面板(EDID)", "Dalle (EDID)"},
    {"ht_scr_edid_unreadable", "EDID not readable", "EDID 不可读", "EDID illisible"},
    {"ht_scr_fill_test", "Start fill test", "开始纯色填充测试", "Démarrer le test"},
    {"ht_scr_fill_hint",
     "Click / → next color · ← previous · Esc exit  (white→black→red→green→blue→gray→gradient)",
     "单击/→ 下一颜色 · ← 上一颜色 · Esc 退出(白→黑→红→绿→蓝→灰→渐变)",
     "Clic / → couleur suivante · ← précédente · Échap quitter"},
    {"ht_scr_result_name", "Display info", "屏幕检测", "Écran"},

    // --- Camera ---
    {"ht_cam_formats", "Supported modes", "支持的分辨率/帧率", "Modes pris en charge"},
    {"ht_cam_preview", "Live preview", "实时预览", "Aperçu en direct"},
    {"ht_cam_mirror", "Mirror", "镜像", "Miroir"},
    {"ht_cam_snapshot", "Save snapshot (PNG)", "拍摄快照(PNG)",
     "Enregistrer une capture (PNG)"},
    {"ht_cam_snapshot_saved", "Saved: {path}", "已保存:{path}",
     "Enregistré : {path}"},
    {"ht_cam_snapshot_failed", "Failed to save snapshot.", "快照保存失败。",
     "Échec de l'enregistrement."},
    {"ht_cam_stats",
     "Resolution: {res}    Nominal: {nominal} fps    Measured: {measured} fps",
     "分辨率:{res}    标称帧率:{nominal} fps    实测帧率:{measured} fps",
     "Résolution : {res}    Nominal : {nominal} fps    Mesuré : {measured} fps"},
    {"ht_cam_no_device", "No camera detected. Connect a camera and it will appear "
     "here automatically.",
     "未检测到摄像头。接入摄像头后会自动出现在这里。",
     "Aucune caméra détectée. Connectez une caméra et elle apparaîtra ici."},
    {"ht_cam_permission", "Camera access is denied. Allow it in Windows Settings → "
     "Privacy & security → Camera, then reopen this page.",
     "摄像头权限被拒绝。请在 Windows 设置 → 隐私和安全性 → 相机 中允许访问,然后重新打开本页。",
     "L'accès à la caméra est refusé. Autorisez-le dans Paramètres Windows → "
     "Confidentialité → Caméra, puis rouvrez cette page."},
    {"ht_cam_busy", "The camera could not be started — it may be in use by another "
     "application.",
     "无法启动摄像头——可能正被其他应用占用。",
     "Impossible de démarrer la caméra — elle est peut-être utilisée par une autre "
     "application."},
    {"ht_cam_result_name", "Camera test", "摄像头检测", "Test de la caméra"},

    // --- Audio ---
    {"ht_aud_speaker_title", "Speaker — melody test", "扬声器——歌曲测试",
     "Haut-parleur — test mélodie"},
    {"ht_aud_melody_hint", "Plays a short melody (Twinkle Twinkle Little Star). "
     "Use Left / Right / Both to check each channel.",
     "播放一段简单歌曲（小星星）。用左 / 右 / 双声道分别检查各声道。",
     "Joue une courte mélodie (Ah! vous dirai-je, maman). "
     "Utilisez Gauche / Droite / Deux canaux pour vérifier chaque canal."},
    {"ht_aud_mic_title", "Microphone", "麦克风", "Microphone"},
    {"ht_aud_left", "Left channel", "左声道", "Canal gauche"},
    {"ht_aud_right", "Right channel", "右声道", "Canal droit"},
    {"ht_aud_both", "Both channels", "双声道", "Deux canaux"},
    {"ht_aud_record", "Record 5 s", "录音 5 秒", "Enregistrer 5 s"},
    {"ht_aud_play", "Play back", "回放试听", "Écouter"},
    {"ht_aud_record_hint", "Record five seconds from the microphone, then play it "
     "back to verify the whole chain.",
     "用麦克风录制 5 秒,然后回放试听,验证完整链路。",
     "Enregistrez cinq secondes puis réécoutez pour vérifier toute la chaîne."},
    {"ht_aud_recording", "Recording… {s} s", "录音中……{s} 秒", "Enregistrement… {s} s"},
    {"ht_aud_record_done", "Recording ready — press Play back.", "录音完成——可回放试听。",
     "Enregistrement prêt — appuyez sur Écouter."},
    {"ht_aud_playing", "Playing…", "回放中……", "Lecture…"},
    {"ht_aud_result_name", "Audio test", "音频检测", "Test audio"},

    // --- CPU ---
    {"ht_cpu_info_title", "Processor", "处理器", "Processeur"},
    {"ht_cpu_cores", "{phys} physical cores / {logical} logical processors",
     "{phys} 物理核心 / {logical} 逻辑处理器",
     "{phys} cœurs physiques / {logical} processeurs logiques"},
    {"ht_cpu_base", "Base frequency: {mhz} GHz", "基础频率:{mhz} GHz",
     "Fréquence de base : {mhz} GHz"},
    {"ht_cpu_temp", "Temperature", "温度", "Température"},
    {"ht_cpu_temp_unsupported",
     "not supported on this platform (no ACPI thermal zone / vendor driver)",
     "此平台不支持(无 ACPI 热区/厂商驱动接口)",
     "non pris en charge sur cette plateforme (pas de zone thermique ACPI)"},
    {"ht_cpu_freq_static_note",
     "Per-core frequency comes from the OS power subsystem and has not varied "
     "yet — it may be a static nominal value on this system.",
     "每核频率读自系统电源子系统,目前数值尚未变化——在本机上可能是静态标称值。",
     "La fréquence par cœur provient du sous-système d'alimentation et n'a pas "
     "encore varié — elle peut être une valeur nominale statique sur ce système."},
    {"ht_cpu_bench_title", "Benchmark (fixed deterministic workload)",
     "跑分(固定确定性负载)", "Benchmark (charge fixe déterministe)"},
    {"ht_cpu_bench_start", "Run benchmark (2 × 5 s)", "开始跑分(2 × 5 秒)",
     "Lancer le benchmark (2 × 5 s)"},
    {"ht_cpu_bench_single_running", "Single-thread phase running…",
     "单线程阶段进行中……", "Phase mono-thread en cours…"},
    {"ht_cpu_bench_multi_running", "All-core phase running…",
     "全核心阶段进行中……", "Phase tous-cœurs en cours…"},
    {"ht_cpu_single", "Single-core", "单核分", "Mono-cœur"},
    {"ht_cpu_multi", "Multi-core", "多核分", "Multi-cœur"},
    {"ht_cpu_ratio", "Scaling", "多核加速比", "Facteur d'échelle"},
    {"ht_cpu_bench_note", "Useful for AC-vs-battery or before/after-cleaning "
     "comparisons on this machine.",
     "适合本机对比:插电 vs 电池、清灰前后。",
     "Utile pour comparer secteur/batterie ou avant/après nettoyage sur cette machine."},
    {"ht_cpu_stress_title", "Stress test — throttling detection (60 s all-core)",
     "压力测试——降频检测(60 秒全核满载)",
     "Test de charge — détection de throttling (60 s tous cœurs)"},
    {"ht_cpu_stress_start", "Start 60 s stress test", "开始 60 秒压力测试",
     "Démarrer le test de 60 s"},
    {"ht_cpu_stress_hint", "Runs all logical cores at full load and charts "
     "throughput + frequency; a sustained drop >15% from peak suggests "
     "thermal/power throttling.",
     "全逻辑核心满载运行,同时绘制吞吐量+频率曲线;较峰值持续下降超过 15% 提示散热/供电降频。",
     "Charge tous les cœurs et trace débit + fréquence ; une baisse durable de "
     ">15 % du pic suggère un throttling thermique."},
    {"ht_cpu_stress_running", "Running… {s}/{total} s", "进行中……{s}/{total} 秒",
     "En cours… {s}/{total} s"},
    {"ht_cpu_stress_done", "Stress test finished.", "压力测试完成。",
     "Test de charge terminé."},
    {"ht_cpu_drop", "vs peak: -{pct}%", "较峰值:-{pct}%", "vs pic : -{pct} %"},
    {"ht_cpu_chart_units", "Throughput (units/s)", "吞吐量(单元/秒)", "Débit (unités/s)"},
    {"ht_cpu_chart_freq", "Avg frequency", "平均频率", "Fréquence moy."},
    {"ht_cpu_result_name", "CPU benchmark", "CPU 跑分", "Benchmark CPU"},
    {"ht_cpu_stress_result_name", "CPU stress (60 s)", "CPU 压力测试(60 秒)",
     "Stress CPU (60 s)"},

    {"ht_cpu_quick_bench_title", "CPU benchmark (30 seconds)", "CPU Benchmark（30 秒）", "Benchmark CPU (30 secondes)"},
    {"ht_cpu_quick_bench_start", "Start 30-second CPU test", "开始 30 秒 CPU 测试", "Démarrer le test CPU de 30 secondes"},
    {"ht_cpu_quick_bench_note", "Runs one core for 15 seconds, then all logical processors for 15 seconds. The running test can be cancelled.",
     "先进行 15 秒单核测试，再进行 15 秒全逻辑处理器测试；运行中可以取消。",
     "Teste un cœur pendant 15 secondes, puis tous les processeurs logiques pendant 15 secondes. Le test peut être annulé."},

    // --- GPU ---
    {"ht_gpu_info_title", "Graphics adapter", "显卡", "Carte graphique"},
    {"ht_gpu_vram", "Dedicated VRAM", "专用显存", "VRAM dédiée"},
    {"ht_gpu_driver", "Driver (UMD)", "驱动版本(UMD)", "Pilote (UMD)"},
    {"ht_gpu_usage", "3D engine usage", "3D 引擎占用", "Utilisation moteur 3D"},
    {"ht_gpu_usage_unavailable", "counters unavailable on this system",
     "此系统无法读取 GPU 占用计数器", "compteurs indisponibles sur ce système"},
    {"ht_gpu_temp_note", "GPU temperature requires vendor driver interfaces "
     "(NVAPI/ADL) and is not read by this tool — shown as not supported rather "
     "than estimated.",
     "GPU 温度需要厂商驱动接口(NVAPI/ADL),本工具不读取——如实显示为不支持,而非估算。",
     "La température GPU nécessite les interfaces du fabricant (NVAPI/ADL) ; cet "
     "outil l'affiche comme non prise en charge plutôt que de l'estimer."},
    {"ht_gpu_bench_title", "Benchmark — offscreen D3D11 escalating load",
     "跑分——D3D11 离屏递增负载", "Benchmark — charge D3D11 hors écran"},
    {"ht_gpu_bench_start", "Run GPU benchmark", "开始 GPU 跑分", "Lancer le benchmark GPU"},
    {"ht_gpu_bench_note", "Renders 1280×720 offscreen; the pixel-shader load "
     "doubles until GPU frame time (timestamp queries) exceeds 33 ms, then "
     "sustains 10 s for the score.",
     "以 1280×720 离屏渲染;像素着色器负载逐级翻倍,直到 GPU 帧时间(时间戳查询)超过 "
     "33ms,然后以最后通过的等级持续 10 秒得出分数。",
     "Rendu hors écran 1280×720 ; la charge du pixel shader double jusqu'à ce que "
     "le temps GPU dépasse 33 ms, puis 10 s de charge soutenue pour le score."},
    {"ht_gpu_level", "Load level {level} — {ms} ms/frame ({fps} fps)",
     "负载等级 {level}——{ms} ms/帧({fps} fps)",
     "Niveau {level} — {ms} ms/image ({fps} ips)"},
    {"ht_gpu_sustained", "Sustained run… {s}/10 s ({fps} fps)",
     "持续阶段……{s}/10 秒({fps} fps)", "Charge soutenue… {s}/10 s ({fps} ips)"},
    {"ht_gpu_done", "Benchmark finished.", "跑分完成。", "Benchmark terminé."},
    {"ht_gpu_failed", "Benchmark failed", "跑分失败", "Échec du benchmark"},
    {"ht_gpu_score", "Score: {score} M ops/s (level {level}, {fps} fps sustained)",
     "得分:{score} M ops/s(等级 {level},持续 {fps} fps)",
     "Score : {score} M ops/s (niveau {level}, {fps} ips soutenus)"},
    {"ht_gpu_result_name", "GPU benchmark", "GPU 跑分", "Benchmark GPU"},

    {"ht_gpu_quick_bench_title", "GPU benchmark (30 seconds) — offscreen D3D11", "GPU Benchmark（30 秒，D3D11 离屏）", "Benchmark GPU (30 secondes) — D3D11 hors écran"},
    {"ht_gpu_quick_bench_start", "Start 30-second GPU test", "开始 30 秒 GPU 测试", "Démarrer le test GPU de 30 secondes"},
    {"ht_gpu_quick_bench_note", "Runs a safe offscreen shader workload for 30 seconds. The running test can be cancelled.",
     "运行安全的离屏着色器负载 30 秒；运行中可以取消。",
     "Exécute une charge shader hors écran sûre pendant 30 secondes. Le test peut être annulé."},
    {"ht_gpu_quick_sustained", "Running… {s}/30 s ({fps} fps)",
     "运行中… {s}/30 秒（{fps} fps）", "En cours… {s}/30 s ({fps} ips)"},

    // --- Disk & Memory ---
    {"ht_st_disk_title", "Disk sequential speed", "磁盘顺序读写速度",
     "Vitesse séquentielle du disque"},
    {"ht_st_disk_hint", "Writes then reads a 1 GB test file with the OS cache "
     "bypassed, so the result is the disk's real sequential throughput. The "
     "test file is deleted afterwards.",
     "写入并读取 1 GB 测试文件,绕过系统缓存,结果为磁盘真实顺序吞吐。测试文件随后自动删除。",
     "Écrit puis lit un fichier de 1 Go en contournant le cache : le résultat est "
     "le débit séquentiel réel du disque. Le fichier est supprimé ensuite."},
    {"ht_st_no_volume", "No volume with at least 2 GB free space.",
     "没有可用空间不少于 2 GB 的卷。", "Aucun volume avec au moins 2 Go libres."},
    {"ht_st_writing", "Writing…", "写入中……", "Écriture…"},
    {"ht_st_reading", "Reading…", "读取中……", "Lecture…"},
    {"ht_st_write", "Write", "写入", "Écriture"},
    {"ht_st_read", "Read", "读取", "Lecture"},
    {"ht_st_ram_title", "Memory test", "内存测试", "Test de mémoire"},
    {"ht_st_ram_hint", "Fills the chosen amount of RAM with fixed patterns "
     "(0x00/0xFF/0x55/0xAA) plus an address-derived pass and verifies every "
     "word. Sizes are capped below free RAM to avoid swapping.",
     "以固定模式(0x00/0xFF/0x55/0xAA)加地址派生模式填充所选容量的内存并逐字校验。"
     "可选容量低于空闲内存,避免触发交换。",
     "Remplit la RAM choisie avec des motifs fixes (0x00/0xFF/0x55/0xAA) plus une "
     "passe dérivée de l'adresse et vérifie chaque mot."},
    {"ht_st_ram_ok", "{gb} GB tested × {passes} passes — no errors",
     "已测试 {gb} GB × {passes} 遍——无错误",
     "{gb} Go testés × {passes} passes — aucune erreur"},
    {"ht_st_ram_errors", "{n} verification error(s) detected!",
     "检测到 {n} 处校验错误!", "{n} erreur(s) de vérification détectée(s) !"},
    {"ht_st_ram_alloc_failed", "Could not allocate the requested memory.",
     "无法分配所选容量的内存。", "Impossible d'allouer la mémoire demandée."},
    {"ht_st_disk_result_name", "Disk speed", "磁盘速度", "Vitesse disque"},
    {"ht_st_ram_result_name", "Memory test", "内存测试", "Test mémoire"},

    // --- Battery & Charging ---
    {"ht_nav_power", "Battery & Charging", "电池与充电", "Batterie et charge"},
    {"ht_power_intro_title", "What this checks", "本项检测说明", "Objet du test"},
    {"ht_power_intro",
     "This does not grade battery health — capacity, wear and cycle count are on "
     "the System Summary page. It shows whether the PC is on AC (charger) or on "
     "the battery, the whole-system live power draw, and the battery voltage / "
     "current / wattage. On battery the draw is the computer's real-time "
     "consumption, so you can see at a glance whether it is running normally; "
     "plug/unplug the charger to test a battery, a charging port or a charger.",
     "本项不评估电池健康度——容量、损耗和循环次数请看“系统摘要”页。这里显示电脑当前是"
     "交流(充电器)供电还是电池供电、整机实时功耗,以及电池的电压 / 电流 / 功率。"
     "电池供电时,整机功耗即电脑的实时耗电,可一眼看出是否正常工作;插拔充电器还可测试"
     "电池、充电口或充电器。",
     "Ce test n'évalue pas la santé de la batterie — capacité, usure et cycles "
     "figurent sur la page Résumé système. Il indique si le PC est sur secteur "
     "(chargeur) ou sur batterie, la consommation système en direct, ainsi que la "
     "tension / courant / puissance de la batterie. Sur batterie, cette "
     "consommation est celle du PC en temps réel : on voit d'un coup d'œil s'il "
     "fonctionne normalement ; branchez/débranchez le chargeur pour tester une "
     "batterie, un port de charge ou un chargeur."},
    {"ht_power_source_title", "Power source", "供电来源", "Source d'alimentation"},
    {"ht_power_live_title", "Live measurements (updated every second)",
     "实时测量(每秒更新)", "Mesures en direct (chaque seconde)"},
    {"ht_power_draw_label", "Whole-system power draw (live)", "整机实时功耗",
     "Consommation système (en direct)"},
    {"ht_power_draw_battery_explain",
     "On battery this is the whole computer's live consumption — everything runs "
     "off the battery, so its discharge equals the total draw. A normal laptop "
     "idles around 8-15 W and rises under load; a wildly high or erratic value "
     "means something is not working right.",
     "电池供电时,这就是整台电脑的实时耗电——所有部件都靠电池,放电功率即总功耗。"
     "正常笔记本待机约 8~15 W,负载时上升;数值异常高或忽大忽小,说明有部件不正常。",
     "Sur batterie, c'est la consommation en direct de tout l'ordinateur : tout "
     "fonctionne sur la batterie, sa décharge égale la consommation totale. Un "
     "portable normal consomme ~8-15 W au repos et plus en charge ; une valeur "
     "très élevée ou instable signale un problème."},
    {"ht_power_draw_ac_charging_explain",
     "On AC and charging ({w} W into the battery). Windows does not report the "
     "adapter's total output, so whole-system draw cannot be shown here. Unplug "
     "the charger to read the computer's live consumption on battery.",
     "接电源且正在充电(以 {w} W 给电池充电)。Windows 不提供适配器的总输出,因此这里"
     "无法显示整机功耗。拔掉充电器,用电池供电即可读出整机实时耗电。",
     "Sur secteur et en charge ({w} W vers la batterie). Windows ne fournit pas la "
     "sortie totale de l'adaptateur ; la consommation système ne peut donc pas "
     "être affichée ici. Débranchez le chargeur pour lire la consommation sur batterie."},
    {"ht_power_draw_ac_idle_explain",
     "On AC, Windows does not report the adapter's total output, so whole-system "
     "draw cannot be measured while plugged in. Unplug the charger to read the "
     "computer's live consumption on battery.",
     "接电源时,Windows 不提供适配器的总输出,插着电无法测整机功耗。拔掉充电器,用电池"
     "供电即可读出整机实时耗电。",
     "Sur secteur, Windows ne fournit pas la sortie totale de l'adaptateur : la "
     "consommation système ne peut pas être mesurée branché. Débranchez le "
     "chargeur pour lire la consommation sur batterie."},
    {"ht_power_on_ac", "AC power — charger connected", "交流供电 — 充电器已连接",
     "Secteur — chargeur connecté"},
    {"ht_power_on_battery", "On battery — running unplugged", "电池供电 — 未接充电器",
     "Sur batterie — débranché"},
    {"ht_power_no_battery", "No battery detected — desktop or battery removed.",
     "未检测到电池 —— 台式机或电池已移除。",
     "Aucune batterie détectée — ordinateur de bureau ou batterie retirée."},
    {"ht_power_charging", "Charging", "正在充电", "En charge"},
    {"ht_power_discharging", "Discharging", "正在放电", "En décharge"},
    {"ht_power_not_charging", "Not charging (full or idle)", "未充电(已充满或空闲)",
     "Pas de charge (pleine ou au repos)"},
    {"ht_power_battery_level", "Battery level {pct}%", "电量 {pct}%",
     "Niveau {pct} %"},
    {"ht_power_voltage", "Voltage", "电压", "Tension"},
    {"ht_power_current", "Current", "电流", "Courant"},
    {"ht_power_power", "Power", "功率", "Puissance"},
    {"ht_power_unavailable", "—", "—", "—"},
    {"ht_power_flow_charging", "Charger is delivering {w} W into the battery.",
     "充电器正在以 {w} W 为电池充电。", "Le chargeur fournit {w} W à la batterie."},
    {"ht_power_flow_discharging", "Battery is supplying {w} W to the system.",
     "电池正在以 {w} W 为系统供电。", "La batterie fournit {w} W au système."},
    {"ht_power_flow_idle", "No measurable charge or discharge right now.",
     "当前无明显充放电。", "Aucune charge/décharge mesurable actuellement."},
    {"ht_power_note",
     "Plug and unplug the charger: the source should switch between AC and "
     "Battery, and the wattage should change. Values come from the battery, so a "
     "desktop with no battery shows only the AC state.",
     "插拔充电器:供电来源应在交流与电池之间切换,功率也应随之变化。数值来自电池,因此"
     "没有电池的台式机只显示交流状态。",
     "Branchez et débranchez le chargeur : la source doit basculer entre secteur "
     "et batterie et la puissance doit changer. Les valeurs proviennent de la "
     "batterie ; un ordinateur de bureau sans batterie n'affiche que l'état secteur."},

    // --- Fingerprint ---
    {"ht_nav_fingerprint", "Fingerprint", "指纹检测", "Empreinte digitale"},
    {"ht_fp_intro_title", "What this checks", "本项检测说明", "Objet du test"},
    {"ht_fp_intro",
     "Checks whether the fingerprint reader works — without setting up "
     "fingerprint sign-in. Detection below confirms the sensor and its driver "
     "are present. The live scan then captures a single sample to prove the "
     "reader physically reads a finger; nothing is enrolled, saved or matched.",
     "检测指纹识别器是否正常工作——无需设置指纹登录。下方“检测”确认传感器及其驱动是否存在;"
     "“实时扫描”再采集一次样本,验证识别器能真正读取手指。全程不录入、不保存、不比对任何指纹。",
     "Vérifie si le lecteur d'empreintes fonctionne — sans configurer la connexion "
     "par empreinte. La détection ci-dessous confirme la présence du capteur et de "
     "son pilote. Le scan en direct capture ensuite un seul échantillon pour "
     "prouver que le lecteur lit un doigt ; rien n'est enregistré ni comparé."},
    {"ht_fp_detect_title", "Detection", "检测", "Détection"},
    {"ht_fp_present", "Fingerprint sensor detected — driver responding.",
     "已检测到指纹传感器——驱动正常响应。",
     "Capteur d'empreintes détecté — pilote fonctionnel."},
    {"ht_fp_absent", "No fingerprint sensor detected.", "未检测到指纹传感器。",
     "Aucun capteur d'empreintes détecté."},
    {"ht_fp_absent_detail",
     "The device is absent, disabled in Device Manager, or its biometric driver "
     "is not loaded.",
     "设备不存在、在设备管理器中被禁用,或其生物识别驱动未加载。",
     "Le périphérique est absent, désactivé dans le Gestionnaire de périphériques, "
     "ou son pilote biométrique n'est pas chargé."},
    {"ht_fp_unit_generic", "Fingerprint unit #{id}", "指纹设备 #{id}",
     "Capteur d'empreintes n° {id}"},
    {"ht_fp_scan_title", "Live scan test", "实时扫描测试", "Test de scan en direct"},
    {"ht_fp_scan_hint",
     "Press Start, then place your finger on the sensor. The reader only has to "
     "respond to the touch — no fingerprint data is read, enrolled or saved.",
     "点击开始,然后将手指放在传感器上。只需识别器对触摸有响应即可——不会读取、录入或保存任何指纹数据。",
     "Appuyez sur Démarrer puis posez votre doigt sur le capteur. Le lecteur doit "
     "seulement réagir au contact — aucune donnée d'empreinte n'est lue, "
     "enregistrée ni sauvegardée."},
    {"ht_fp_admin_yes", "Running as administrator — raw capture is permitted.",
     "已以管理员身份运行——允许进行原始采集。",
     "Exécuté en administrateur — la capture brute est autorisée."},
    {"ht_fp_admin_no",
     "Not running as administrator — the live scan will likely be refused. "
     "Restart the app (it requests administrator rights on launch).",
     "未以管理员身份运行——实时扫描很可能被拒绝。请重启本应用(启动时会请求管理员权限)。",
     "Pas exécuté en administrateur — le scan en direct sera probablement refusé. "
     "Redémarrez l'application (elle demande les droits administrateur au lancement)."},
    {"ht_fp_start_btn", "Start scan", "开始扫描", "Démarrer le scan"},
    {"ht_fp_scanning", "Place your finger on the sensor…", "请将手指放在传感器上……",
     "Posez votre doigt sur le capteur…"},
    {"ht_fp_captured", "The sensor responded to your finger — the scanner is working.",
     "传感器已响应你的手指——识别器工作正常。",
     "Le capteur a réagi à votre doigt — le lecteur fonctionne."},
    {"ht_fp_err_access",
     "The live scan needs the app run as administrator. The sensor was still "
     "detected above, so the hardware and driver are present.",
     "实时扫描需要以管理员身份运行本应用。上方已检测到传感器,说明硬件和驱动均已就绪。",
     "Le scan en direct nécessite d'exécuter l'application en administrateur. Le "
     "capteur a néanmoins été détecté ci-dessus : matériel et pilote sont présents."},
    {"ht_fp_err_notimpl",
     "This sensor does not allow software to read it directly — common on newer "
     "laptops with a secure (match-on-chip / Enhanced Sign-in Security) reader. "
     "Detection above already confirms the sensor and driver are present and "
     "healthy; the only way to functionally confirm it is to enroll a fingerprint "
     "in Windows Hello.",
     "该传感器不允许软件直接读取——这在带安全识别器(match-on-chip / 增强登录安全)的新款"
     "笔记本上很常见。上方“检测”已确认传感器与驱动存在且正常;要进一步功能验证,只能在 "
     "Windows Hello 中录入一枚指纹。",
     "Ce capteur n'autorise pas la lecture directe par logiciel — fréquent sur les "
     "ordinateurs portables récents à lecteur sécurisé (match-on-chip / sécurité "
     "de connexion renforcée). La détection ci-dessus confirme déjà que le capteur "
     "et le pilote sont présents et sains ; la seule confirmation fonctionnelle "
     "est d'enregistrer une empreinte dans Windows Hello."},
    {"ht_fp_err_refused",
     "The sensor refused the capture even though the app is running as "
     "administrator. Its driver may not allow raw capture, or Windows Hello is "
     "holding the sensor — close any sign-in prompt and try again.",
     "尽管本应用已以管理员身份运行,传感器仍拒绝了采集。可能是其驱动不允许原始采集,"
     "或 Windows Hello 正占用传感器——请关闭任何登录提示后重试。",
     "Le capteur a refusé la capture bien que l'application soit en administrateur. "
     "Son pilote n'autorise peut-être pas la capture brute, ou Windows Hello "
     "occupe le capteur — fermez toute invite de connexion et réessayez."},
    {"ht_fp_err_busy",
     "The sensor is busy — another app or Windows Hello is using it. Close it and "
     "try again.",
     "传感器正被占用——其他应用或 Windows Hello 正在使用。请关闭后重试。",
     "Le capteur est occupé — une autre application ou Windows Hello l'utilise. "
     "Fermez-la puis réessayez."},
    {"ht_fp_err_cancelled", "Scan cancelled.", "扫描已取消。", "Scan annulé."},
    {"ht_fp_err_nosensor",
     "The sensor became unavailable. Reconnect it and reopen this page.",
     "传感器已不可用。请重新连接后重新打开本页。",
     "Le capteur est devenu indisponible. Reconnectez-le et rouvrez cette page."},
    {"ht_fp_err_other", "The scan could not complete (error {code}).",
     "扫描无法完成(错误 {code})。", "Le scan n'a pas pu aboutir (erreur {code})."},
    {"ht_fp_result_name", "Fingerprint scanner", "指纹识别", "Lecteur d'empreintes"},
};
} // namespace

// Inserts every row into the per-language maps (see the header).
void addTesterTranslations(QMap<QString, QMap<QString, QString>> &translations) {
    QMap<QString, QString> &en = translations["en"];
    QMap<QString, QString> &zh = translations["zh"];
    QMap<QString, QString> &fr = translations["fr"];
    for (const Row &r : kRows) {
        Q_ASSERT(r.en && r.zh && r.fr); // every key must exist in all 3 languages
        en.insert(QString::fromUtf8(r.key), QString::fromUtf8(r.en));
        zh.insert(QString::fromUtf8(r.key), QString::fromUtf8(r.zh));
        fr.insert(QString::fromUtf8(r.key), QString::fromUtf8(r.fr));
    }
}
