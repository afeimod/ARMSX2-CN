// SettingsStore.swift — INI-backed settings for SwiftUI
// SPDX-License-Identifier: GPL-3.0+

import Foundation
import SwiftUI

/// [P51] OSD preset levels
enum OsdPreset: Int, CaseIterable {
    case off = 0
    case simple = 1    // FPS + CPU usage
    case detail = 2    // All except frame times graph
    case full = 3      // Everything

    var label: String {
        switch self {
        case .off: return "OFF"
        case .simple: return "Simple"
        case .detail: return "Detail"
        case .full: return "Full"
        }
    }
}

enum VirtualPadSkin: Int, CaseIterable, Identifiable {
    case armsx2Refresh = 0
    case crispVector = 1
    case custom = 2

    var id: Int { rawValue }

    var label: String {
        switch self {
        case .armsx2Refresh:
            return "ARMSX2 Refresh"
        case .crispVector:
            return "Crisp Vector"
        case .custom:
            return "Custom Imported"
        }
    }

    var detail: String {
        switch self {
        case .armsx2Refresh:
            return "Uses the bundled ARMSX2 refresh button art with stronger press feedback."
        case .crispVector:
            return "Draws the pad in SwiftUI for sharper outlines at any screen scale."
        case .custom:
            return "Loads user-imported button images or a full portrait/landscape skin from the custom skin folder."
        }
    }

    static func customSkinDirectory(create: Bool = false) -> URL? {
        guard let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first else {
            return nil
        }

        let directory = documents.appendingPathComponent("ControllerSkins/Custom", isDirectory: true)
        if create {
            try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        }
        return directory
    }
}

enum AppLanguage: String, CaseIterable, Identifiable {
    case system
    case english
    case simplifiedChinese
    case arabic
    case spanish
    case french
    case portuguese
    case japanese
    case korean

    var id: String { rawValue }

    var label: String {
        switch self {
        case .system: return "System Default"
        case .english: return "English"
        case .simplifiedChinese: return "简体中文"
        case .arabic: return "العربية"
        case .spanish: return "Español"
        case .french: return "Français"
        case .portuguese: return "Português"
        case .japanese: return "日本語"
        case .korean: return "한국어"
        }
    }

    static func resolvedSystemLanguage() -> AppLanguage {
        let code = Locale.current.language.languageCode?.identifier.lowercased() ?? "en"
        switch code {
        case "zh":
            return .simplifiedChinese
        case "ar":
            return .arabic
        case "es":
            return .spanish
        case "fr":
            return .french
        case "pt":
            return .portuguese
        case "ja":
            return .japanese
        case "ko":
            return .korean
        default:
            return .english
        }
    }

    var resolved: AppLanguage {
        self == .system ? Self.resolvedSystemLanguage() : self
    }

    var layoutDirection: LayoutDirection {
        resolved == .arabic ? .rightToLeft : .leftToRight
    }

    func localized(_ key: String) -> String {
        guard let translated = Self.translations[resolved]?[key] else {
            return key
        }
        return translated
    }

    private static let translations: [AppLanguage: [String: String]] = [
        .simplifiedChinese: [
            "System Default": "跟随系统",
            "Language": "语言",
            "Interface Language": "界面语言",
            "App Language": "应用语言",
            "ARMSX2 iOS menus will use this language where available. Some emulator terms and debug messages may still appear in English.": "ARMSX2 iOS 菜单会尽量使用此语言。部分模拟器术语和调试消息可能仍以英文显示。",
            "Games": "游戏",
            "BIOS": "BIOS",
            "Help": "帮助",
            "Settings": "设置",
            "File Import": "文件导入",
            "Import Result": "导入结果",
            "Cover Result": "封面结果",
            "Cover Source": "封面来源",
            "OK": "确定",
            "Cancel": "取消",
            "Save": "保存",
            "Done": "完成",
            "Restart": "重启",
            "Stop": "停止",
            "Resume": "继续",
            "Version": "版本",
            "About": "关于",
            "Import Games": "导入游戏",
            "Import BIOS": "导入 BIOS",
            "BIOS Only": "仅启动 BIOS",
            "No Games Found": "未找到游戏",
            "Import PS2 disc images to add them here.": "导入 PS2 光盘镜像后会显示在这里。",
            "No BIOS Found": "未找到 BIOS",
            "Import a PS2 BIOS dump to enable booting.": "导入 PS2 BIOS 转储文件以启用启动。",
            "Compatibility Picker": "兼容选择器",
            "If one picker refuses to select your .bin/.rom file, try the other.": "如果一个选择器无法选择 .bin/.rom 文件，请尝试另一个。",
            "Emulator": "模拟器",
            "Graphics": "图形",
            "Network": "网络",
            "Memory Cards": "记忆卡",
            "Storage": "存储",
            "RetroAchievements": "RetroAchievements",
            "Overlay (OSD)": "叠加层 (OSD)",
            "Game Controller": "游戏手柄",
            "Local Multiplayer": "本地多人",
            "Virtual Pad": "虚拟按键",
            "Licenses & Credits": "许可证与鸣谢",
            "Performance Overlay": "性能叠加层",
            "Preset": "预设",
            "Position": "位置",
            "Hidden": "隐藏",
            "Top Left": "左上",
            "Top Right": "右上",
            "Displayed Items": "显示项目",
            "Show FPS": "显示 FPS",
            "Show VPS": "显示 VPS",
            "Show Speed": "显示速度",
            "Show CPU": "显示 CPU",
            "Show GPU": "显示 GPU",
            "Show Resolution": "显示分辨率",
            "Show GS Stats": "显示 GS 统计",
            "Show Indicators": "显示指示器",
            "Show Settings": "显示设置",
            "Show Inputs": "显示输入",
            "Show Frame Times": "显示帧时间",
            "Show Version": "显示版本",
            "Show Hardware Info": "显示硬件信息",
            "Show Device Stats": "显示设备状态",
            "Notes": "说明",
            "Overlay": "叠加层",
            "Appearance": "外观",
            "Button Skin": "按键皮肤",
            "Custom Skin": "自定义皮肤",
            "Import Button Images": "导入按键图片",
            "Feedback": "反馈",
            "Haptic Feedback": "触觉反馈",
            "Layout": "布局",
            "Edit Layout": "编辑布局",
            "Drag buttons to reposition. Pinch to resize.": "拖动按键调整位置，捏合调整大小。",
            "Custom Imported": "自定义导入",
            "Crisp Vector": "清晰矢量",
            "ARMSX2 Refresh": "ARMSX2 Refresh",
            "Settings Guide": "设置指南",
            "Supported Formats": "支持格式"
        ],
        .arabic: [
            "System Default": "حسب النظام",
            "Language": "اللغة",
            "Interface Language": "لغة الواجهة",
            "App Language": "لغة التطبيق",
            "ARMSX2 iOS menus will use this language where available. Some emulator terms and debug messages may still appear in English.": "ستستخدم قوائم ARMSX2 iOS هذه اللغة عند توفرها. قد تظل بعض مصطلحات المحاكي ورسائل التصحيح باللغة الإنجليزية.",
            "Games": "الألعاب",
            "BIOS": "BIOS",
            "Help": "مساعدة",
            "Settings": "الإعدادات",
            "File Import": "استيراد ملف",
            "Import Result": "نتيجة الاستيراد",
            "Cover Result": "نتيجة الغلاف",
            "Cover Source": "مصدر الأغلفة",
            "OK": "موافق",
            "Cancel": "إلغاء",
            "Save": "حفظ",
            "Done": "تم",
            "Restart": "إعادة تشغيل",
            "Stop": "إيقاف",
            "Resume": "استئناف",
            "Version": "الإصدار",
            "About": "حول",
            "Import Games": "استيراد الألعاب",
            "Import BIOS": "استيراد BIOS",
            "BIOS Only": "BIOS فقط",
            "No Games Found": "لم يتم العثور على ألعاب",
            "Import PS2 disc images to add them here.": "استورد صور أقراص PS2 لإضافتها هنا.",
            "No BIOS Found": "لم يتم العثور على BIOS",
            "Import a PS2 BIOS dump to enable booting.": "استورد ملف BIOS من جهاز PS2 لتفعيل التشغيل.",
            "Compatibility Picker": "منتقي التوافق",
            "If one picker refuses to select your .bin/.rom file, try the other.": "إذا رفض أحد المنتقيات اختيار ملف .bin/.rom، جرّب الآخر.",
            "Emulator": "المحاكي",
            "Graphics": "الرسوميات",
            "Network": "الشبكة",
            "Memory Cards": "بطاقات الذاكرة",
            "Storage": "التخزين",
            "RetroAchievements": "RetroAchievements",
            "Overlay (OSD)": "التراكب (OSD)",
            "Game Controller": "يد التحكم",
            "Local Multiplayer": "لعب محلي متعدد",
            "Virtual Pad": "لوحة افتراضية",
            "Licenses & Credits": "التراخيص والشكر",
            "Performance Overlay": "تراكب الأداء",
            "Preset": "الإعداد المسبق",
            "Position": "الموضع",
            "Hidden": "مخفي",
            "Top Left": "أعلى اليسار",
            "Top Right": "أعلى اليمين",
            "Displayed Items": "العناصر المعروضة",
            "Show FPS": "إظهار FPS",
            "Show VPS": "إظهار VPS",
            "Show Speed": "إظهار السرعة",
            "Show CPU": "إظهار CPU",
            "Show GPU": "إظهار GPU",
            "Show Resolution": "إظهار الدقة",
            "Show GS Stats": "إظهار إحصاءات GS",
            "Show Indicators": "إظهار المؤشرات",
            "Show Settings": "إظهار الإعدادات",
            "Show Inputs": "إظهار الإدخال",
            "Show Frame Times": "إظهار أزمنة الإطارات",
            "Show Version": "إظهار الإصدار",
            "Show Hardware Info": "إظهار معلومات العتاد",
            "Show Device Stats": "إظهار حالة الجهاز",
            "Notes": "ملاحظات",
            "Overlay": "التراكب",
            "Appearance": "المظهر",
            "Button Skin": "مظهر الأزرار",
            "Custom Skin": "مظهر مخصص",
            "Import Button Images": "استيراد صور الأزرار",
            "Feedback": "التغذية الراجعة",
            "Haptic Feedback": "اهتزاز لمسي",
            "Layout": "التخطيط",
            "Edit Layout": "تعديل التخطيط",
            "Drag buttons to reposition. Pinch to resize.": "اسحب الأزرار لتغيير موضعها، واقرص لتغيير الحجم.",
            "Custom Imported": "مخصص مستورد",
            "Crisp Vector": "متجه حاد",
            "ARMSX2 Refresh": "ARMSX2 Refresh",
            "Settings Guide": "دليل الإعدادات",
            "Supported Formats": "الصيغ المدعومة"
        ],
        .spanish: [
            "System Default": "Predeterminado del sistema",
            "Language": "Idioma",
            "Interface Language": "Idioma de la interfaz",
            "App Language": "Idioma de la app",
            "ARMSX2 iOS menus will use this language where available. Some emulator terms and debug messages may still appear in English.": "Los menús de ARMSX2 iOS usarán este idioma cuando esté disponible. Algunos términos del emulador y mensajes de depuración pueden seguir apareciendo en inglés.",
            "Games": "Juegos",
            "BIOS": "BIOS",
            "Help": "Ayuda",
            "Settings": "Ajustes",
            "File Import": "Importar archivo",
            "Import Result": "Resultado de importación",
            "Cover Result": "Resultado de carátula",
            "Cover Source": "Fuente de carátulas",
            "OK": "Aceptar",
            "Cancel": "Cancelar",
            "Save": "Guardar",
            "Done": "Listo",
            "Restart": "Reiniciar",
            "Stop": "Detener",
            "Resume": "Continuar",
            "Version": "Versión",
            "About": "Acerca de",
            "Import Games": "Importar juegos",
            "Import BIOS": "Importar BIOS",
            "BIOS Only": "Solo BIOS",
            "No Games Found": "No se encontraron juegos",
            "Import PS2 disc images to add them here.": "Importa imágenes de disco de PS2 para añadirlas aquí.",
            "No BIOS Found": "No se encontró BIOS",
            "Import a PS2 BIOS dump to enable booting.": "Importa una copia de BIOS de PS2 para poder iniciar.",
            "Compatibility Picker": "Selector de compatibilidad",
            "If one picker refuses to select your .bin/.rom file, try the other.": "Si un selector no permite elegir tu .bin/.rom, prueba el otro.",
            "Emulator": "Emulador",
            "Graphics": "Gráficos",
            "Network": "Red",
            "Memory Cards": "Tarjetas de memoria",
            "Storage": "Almacenamiento",
            "RetroAchievements": "RetroAchievements",
            "Overlay (OSD)": "Superposición (OSD)",
            "Game Controller": "Mando",
            "Local Multiplayer": "Multijugador local",
            "Virtual Pad": "Controles táctiles",
            "Licenses & Credits": "Licencias y créditos",
            "Performance Overlay": "Superposición de rendimiento",
            "Preset": "Preset",
            "Position": "Posición",
            "Hidden": "Oculto",
            "Top Left": "Arriba izquierda",
            "Top Right": "Arriba derecha",
            "Displayed Items": "Elementos mostrados",
            "Show FPS": "Mostrar FPS",
            "Show VPS": "Mostrar VPS",
            "Show Speed": "Mostrar velocidad",
            "Show CPU": "Mostrar CPU",
            "Show GPU": "Mostrar GPU",
            "Show Resolution": "Mostrar resolución",
            "Show GS Stats": "Mostrar estadísticas GS",
            "Show Indicators": "Mostrar indicadores",
            "Show Settings": "Mostrar ajustes",
            "Show Inputs": "Mostrar entradas",
            "Show Frame Times": "Mostrar tiempos de fotograma",
            "Show Version": "Mostrar versión",
            "Show Hardware Info": "Mostrar hardware",
            "Show Device Stats": "Mostrar estado del dispositivo",
            "Notes": "Notas",
            "Overlay": "Superposición",
            "Appearance": "Apariencia",
            "Button Skin": "Tema de botones",
            "Custom Skin": "Tema personalizado",
            "Import Button Images": "Importar imágenes de botones",
            "Feedback": "Respuesta",
            "Haptic Feedback": "Respuesta háptica",
            "Layout": "Diseño",
            "Edit Layout": "Editar diseño",
            "Drag buttons to reposition. Pinch to resize.": "Arrastra los botones para moverlos. Pellizca para cambiar tamaño.",
            "Custom Imported": "Personalizado importado",
            "Crisp Vector": "Vector nítido",
            "ARMSX2 Refresh": "ARMSX2 Refresh",
            "Settings Guide": "Guía de ajustes",
            "Supported Formats": "Formatos compatibles"
        ],
        .french: [
            "System Default": "Langue du système",
            "Language": "Langue",
            "Interface Language": "Langue de l'interface",
            "App Language": "Langue de l'app",
            "ARMSX2 iOS menus will use this language where available. Some emulator terms and debug messages may still appear in English.": "Les menus d'ARMSX2 iOS utiliseront cette langue si disponible. Certains termes de l'émulateur et messages de débogage peuvent rester en anglais.",
            "Games": "Jeux",
            "BIOS": "BIOS",
            "Help": "Aide",
            "Settings": "Réglages",
            "OK": "OK",
            "Cancel": "Annuler",
            "Save": "Enregistrer",
            "Done": "Terminé",
            "Restart": "Redémarrer",
            "Stop": "Arrêter",
            "Resume": "Reprendre",
            "Version": "Version",
            "About": "À propos",
            "Import Games": "Importer des jeux",
            "Import BIOS": "Importer le BIOS",
            "No Games Found": "Aucun jeu trouvé",
            "No BIOS Found": "Aucun BIOS trouvé",
            "Emulator": "Émulateur",
            "Graphics": "Graphismes",
            "Network": "Réseau",
            "Memory Cards": "Cartes mémoire",
            "Storage": "Stockage",
            "Overlay (OSD)": "Superposition (OSD)",
            "Game Controller": "Manette",
            "Local Multiplayer": "Multijoueur local",
            "Virtual Pad": "Commandes tactiles",
            "Licenses & Credits": "Licences et crédits",
            "Performance Overlay": "Superposition de performance",
            "Preset": "Préréglage",
            "Position": "Position",
            "Hidden": "Masqué",
            "Top Left": "En haut à gauche",
            "Top Right": "En haut à droite",
            "Displayed Items": "Éléments affichés",
            "Show FPS": "Afficher FPS",
            "Show CPU": "Afficher CPU",
            "Show GPU": "Afficher GPU",
            "Show Device Stats": "Afficher l'état de l'appareil",
            "Overlay": "Superposition",
            "Appearance": "Apparence",
            "Button Skin": "Thème des boutons",
            "Custom Skin": "Thème personnalisé",
            "Import Button Images": "Importer des images de boutons",
            "Haptic Feedback": "Retour haptique",
            "Edit Layout": "Modifier la disposition"
        ],
        .portuguese: [
            "System Default": "Padrão do sistema",
            "Language": "Idioma",
            "Interface Language": "Idioma da interface",
            "App Language": "Idioma do app",
            "ARMSX2 iOS menus will use this language where available. Some emulator terms and debug messages may still appear in English.": "Os menus do ARMSX2 iOS usarão este idioma quando disponível. Alguns termos do emulador e mensagens de depuração ainda podem aparecer em inglês.",
            "Games": "Jogos",
            "BIOS": "BIOS",
            "Help": "Ajuda",
            "Settings": "Configurações",
            "OK": "OK",
            "Cancel": "Cancelar",
            "Save": "Salvar",
            "Done": "Concluído",
            "Restart": "Reiniciar",
            "Stop": "Parar",
            "Resume": "Continuar",
            "Version": "Versão",
            "About": "Sobre",
            "Import Games": "Importar jogos",
            "Import BIOS": "Importar BIOS",
            "No Games Found": "Nenhum jogo encontrado",
            "No BIOS Found": "Nenhuma BIOS encontrada",
            "Emulator": "Emulador",
            "Graphics": "Gráficos",
            "Network": "Rede",
            "Memory Cards": "Cartões de memória",
            "Storage": "Armazenamento",
            "Overlay (OSD)": "Sobreposição (OSD)",
            "Game Controller": "Controle",
            "Local Multiplayer": "Multijogador local",
            "Virtual Pad": "Controles virtuais",
            "Licenses & Credits": "Licenças e créditos",
            "Performance Overlay": "Sobreposição de desempenho",
            "Preset": "Predefinição",
            "Position": "Posição",
            "Hidden": "Oculto",
            "Top Left": "Superior esquerdo",
            "Top Right": "Superior direito",
            "Displayed Items": "Itens exibidos",
            "Show FPS": "Mostrar FPS",
            "Show CPU": "Mostrar CPU",
            "Show GPU": "Mostrar GPU",
            "Show Device Stats": "Mostrar status do dispositivo",
            "Overlay": "Sobreposição",
            "Appearance": "Aparência",
            "Button Skin": "Tema dos botões",
            "Custom Skin": "Tema personalizado",
            "Import Button Images": "Importar imagens dos botões",
            "Haptic Feedback": "Feedback tátil",
            "Edit Layout": "Editar layout"
        ],
        .japanese: [
            "System Default": "システム設定",
            "Language": "言語",
            "Interface Language": "インターフェース言語",
            "App Language": "アプリの言語",
            "ARMSX2 iOS menus will use this language where available. Some emulator terms and debug messages may still appear in English.": "ARMSX2 iOS のメニューは、利用可能な場合この言語を使用します。一部のエミュレーター用語やデバッグメッセージは英語のまま表示される場合があります。",
            "Games": "ゲーム",
            "BIOS": "BIOS",
            "Help": "ヘルプ",
            "Settings": "設定",
            "OK": "OK",
            "Cancel": "キャンセル",
            "Save": "保存",
            "Done": "完了",
            "Restart": "再起動",
            "Stop": "停止",
            "Resume": "再開",
            "Version": "バージョン",
            "About": "情報",
            "Import Games": "ゲームをインポート",
            "Import BIOS": "BIOSをインポート",
            "No Games Found": "ゲームが見つかりません",
            "No BIOS Found": "BIOSが見つかりません",
            "Emulator": "エミュレーター",
            "Graphics": "グラフィック",
            "Network": "ネットワーク",
            "Memory Cards": "メモリーカード",
            "Storage": "ストレージ",
            "Overlay (OSD)": "オーバーレイ (OSD)",
            "Game Controller": "ゲームコントローラー",
            "Local Multiplayer": "ローカルマルチプレイ",
            "Virtual Pad": "仮想パッド",
            "Licenses & Credits": "ライセンスとクレジット",
            "Performance Overlay": "パフォーマンス表示",
            "Preset": "プリセット",
            "Position": "位置",
            "Hidden": "非表示",
            "Top Left": "左上",
            "Top Right": "右上",
            "Displayed Items": "表示項目",
            "Show FPS": "FPSを表示",
            "Show CPU": "CPUを表示",
            "Show GPU": "GPUを表示",
            "Show Device Stats": "デバイス状態を表示",
            "Overlay": "オーバーレイ",
            "Appearance": "外観",
            "Button Skin": "ボタンスキン",
            "Custom Skin": "カスタムスキン",
            "Import Button Images": "ボタン画像をインポート",
            "Haptic Feedback": "触覚フィードバック",
            "Edit Layout": "レイアウト編集"
        ],
        .korean: [
            "System Default": "시스템 기본값",
            "Language": "언어",
            "Interface Language": "인터페이스 언어",
            "App Language": "앱 언어",
            "ARMSX2 iOS menus will use this language where available. Some emulator terms and debug messages may still appear in English.": "ARMSX2 iOS 메뉴는 가능한 경우 이 언어를 사용합니다. 일부 에뮬레이터 용어와 디버그 메시지는 영어로 표시될 수 있습니다.",
            "Games": "게임",
            "BIOS": "BIOS",
            "Help": "도움말",
            "Settings": "설정",
            "OK": "확인",
            "Cancel": "취소",
            "Save": "저장",
            "Done": "완료",
            "Restart": "재시작",
            "Stop": "중지",
            "Resume": "재개",
            "Version": "버전",
            "About": "정보",
            "Import Games": "게임 가져오기",
            "Import BIOS": "BIOS 가져오기",
            "No Games Found": "게임을 찾을 수 없음",
            "No BIOS Found": "BIOS를 찾을 수 없음",
            "Emulator": "에뮬레이터",
            "Graphics": "그래픽",
            "Network": "네트워크",
            "Memory Cards": "메모리 카드",
            "Storage": "저장 공간",
            "Overlay (OSD)": "오버레이 (OSD)",
            "Game Controller": "게임 컨트롤러",
            "Local Multiplayer": "로컬 멀티플레이",
            "Virtual Pad": "가상 패드",
            "Licenses & Credits": "라이선스 및 크레딧",
            "Performance Overlay": "성능 오버레이",
            "Preset": "프리셋",
            "Position": "위치",
            "Hidden": "숨김",
            "Top Left": "왼쪽 위",
            "Top Right": "오른쪽 위",
            "Displayed Items": "표시 항목",
            "Show FPS": "FPS 표시",
            "Show CPU": "CPU 표시",
            "Show GPU": "GPU 표시",
            "Show Device Stats": "기기 상태 표시",
            "Overlay": "오버레이",
            "Appearance": "모양",
            "Button Skin": "버튼 스킨",
            "Custom Skin": "사용자 스킨",
            "Import Button Images": "버튼 이미지 가져오기",
            "Haptic Feedback": "햅틱 피드백",
            "Edit Layout": "레이아웃 편집"
        ]
    ]
}

@Observable
final class SettingsStore: @unchecked Sendable {
    static let shared = SettingsStore()
    static let minTargetFPS: Float = 15.0
    static let maxTargetFPS: Float = 120.0
    static let defaultTargetFPS: Float = 60.0

    @ObservationIgnored private var suppressINIWrites = false

    // ── Emulator / CPU ──
    var eeCoreType: Int {
        didSet {
            ARMSX2Bridge.setINIInt("EmuCore/CPU", key: "CoreType", value: Int32(eeCoreType))
            ARMSX2Bridge.setINIBool("EmuCore/CPU", key: "UseArm64Dynarec", value: eeCoreType == 2)
        }
    }
    var iopRecompiler: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/CPU/Recompiler", key: "EnableIOP", value: iopRecompiler) }
    }
    var vu0Recompiler: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/CPU/Recompiler", key: "EnableVU0", value: vu0Recompiler) }
    }
    var vu1Recompiler: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/CPU/Recompiler", key: "EnableVU1", value: vu1Recompiler) }
    }
    var fastBoot: Bool {
        didSet {
            ARMSX2Bridge.setINIBool("GameISO", key: "FastBoot", value: fastBoot)
            ARMSX2Bridge.setINIBool("EmuCore", key: "EnableFastBoot", value: fastBoot)
        }
    }
    var fastmem: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/CPU/Recompiler", key: "EnableFastmem", value: fastmem) }
    }
    var frameLimiterEnabled: Bool {
        didSet { applyFrameLimiterSettings() }
    }
    var targetFPS: Float {
        didSet {
            let normalized = Self.clampedTargetFPS(targetFPS)
            guard abs(targetFPS - normalized) <= 0.001 else {
                targetFPS = normalized
                return
            }
            applyFrameLimiterSettings()
        }
    }
    var ntscFramerate: Float {
        didSet {
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIFloat("EmuCore/GS", key: "FramerateNTSC", value: ntscFramerate)
            applyFrameLimiterSettings()
        }
    }
    var palFramerate: Float {
        didSet {
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIFloat("EmuCore/GS", key: "FrameratePAL", value: palFramerate)
        }
    }

    // ── Boot ──
    var fastCDVD: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/Speedhacks", key: "fastCDVD", value: fastCDVD) }
    }

    // ── Advanced Speedhacks ──
    var eeCycleRate: Int {
        didSet { ARMSX2Bridge.setINIInt("EmuCore/Speedhacks", key: "EECycleRate", value: Int32(eeCycleRate)) }
    }
    var vu1Instant: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/Speedhacks", key: "vu1Instant", value: vu1Instant) }
    }
    var mtvu: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/Speedhacks", key: "vuThread", value: mtvu) }
    }
    var waitLoop: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/Speedhacks", key: "WaitLoop", value: waitLoop) }
    }
    var intcStat: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/Speedhacks", key: "IntcStat", value: intcStat) }
    }
    var enableCheats: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore", key: "EnableCheats", value: enableCheats) }
    }
    var enablePatches: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore", key: "EnablePatches", value: enablePatches) }
    }
    var enableGameFixes: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore", key: "EnableGameFixes", value: enableGameFixes) }
    }
    var enableGameDBHardwareFixes: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "UserHacks", value: !enableGameDBHardwareFixes) }
    }
    var enableWidescreenPatches: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore", key: "EnableWideScreenPatches", value: enableWidescreenPatches) }
    }
    var enableNoInterlacingPatches: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore", key: "EnableNoInterlacingPatches", value: enableNoInterlacingPatches) }
    }

    // ── Graphics ──
    var renderer: Int {
        didSet { ARMSX2Bridge.setINIInt("EmuCore/GS", key: "Renderer", value: Int32(renderer)) }
    }
    var upscaleMultiplier: Float {
        didSet { ARMSX2Bridge.setINIFloat("EmuCore/GS", key: "upscale_multiplier", value: upscaleMultiplier) }
    }
    var vsyncQueueSize: Int {
        didSet { ARMSX2Bridge.setINIInt("EmuCore/GS", key: "VsyncQueueSize", value: Int32(vsyncQueueSize)) }
    }
    var textureFiltering: Int {
        didSet { ARMSX2Bridge.setINIInt("EmuCore/GS", key: "filter", value: Int32(textureFiltering)) }
    }
    var hardwareMipmapping: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "hw_mipmap", value: hardwareMipmapping) }
    }
    var fxaa: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "fxaa", value: fxaa) }
    }
    var casMode: Int {
        didSet { ARMSX2Bridge.setINIInt("EmuCore/GS", key: "CASMode", value: Int32(casMode)) }
    }
    var casSharpness: Int {
        didSet { ARMSX2Bridge.setINIInt("EmuCore/GS", key: "CASSharpness", value: Int32(casSharpness)) }
    }
    var interlaceMode: Int {
        didSet { ARMSX2Bridge.setINIInt("EmuCore/GS", key: "deinterlace_mode", value: Int32(interlaceMode)) }
    }
    var aspectRatio: Int {
        didSet { ARMSX2Bridge.setINIString("EmuCore/GS", key: "AspectRatio", value: Self.aspectRatioName(for: aspectRatio)) }
    }
    var blendingAccuracy: Int {
        didSet { ARMSX2Bridge.setINIInt("EmuCore/GS", key: "accurate_blending_unit", value: Int32(blendingAccuracy)) }
    }
    var dithering: Int {
        didSet { ARMSX2Bridge.setINIInt("EmuCore/GS", key: "dithering_ps2", value: Int32(dithering)) }
    }
    var loadTextureReplacements: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "LoadTextureReplacements", value: loadTextureReplacements) }
    }
    var loadTextureReplacementsAsync: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "LoadTextureReplacementsAsync", value: loadTextureReplacementsAsync) }
    }
    var precacheTextureReplacements: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "PrecacheTextureReplacements", value: precacheTextureReplacements) }
    }
    var texturePreloading: Int {
        didSet { ARMSX2Bridge.setINIInt("EmuCore/GS", key: "texture_preloading", value: Int32(texturePreloading)) }
    }
    var dumpReplaceableTextures: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "DumpReplaceableTextures", value: dumpReplaceableTextures) }
    }
    var dumpReplaceableMipmaps: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "DumpReplaceableMipmaps", value: dumpReplaceableMipmaps) }
    }
    var dumpTexturesWithFMVActive: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "DumpTexturesWithFMVActive", value: dumpTexturesWithFMVActive) }
    }
    var dumpDirectTextures: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "DumpDirectTextures", value: dumpDirectTextures) }
    }
    var dumpPaletteTextures: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "DumpPaletteTextures", value: dumpPaletteTextures) }
    }

    // ── OSD Overlay ──
    var osdPreset: OsdPreset {
        didSet {
            ARMSX2Bridge.setINIInt("ARMSX2iOS/UI", key: "OsdPreset", value: Int32(osdPreset.rawValue))
            applyOsdPreset(osdPreset)
        }
    }
    var osdPerformancePosition: Int {
        didSet { ARMSX2Bridge.setINIInt("EmuCore/GS", key: "OsdPerformancePos", value: Int32(osdPerformancePosition)) }
    }
    var osdShowFPS: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "OsdShowFPS", value: osdShowFPS) }
    }
    var osdShowVPS: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "OsdShowVPS", value: osdShowVPS) }
    }
    var osdShowSpeed: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "OsdShowSpeed", value: osdShowSpeed) }
    }
    var osdShowCPU: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "OsdShowCPU", value: osdShowCPU) }
    }
    var osdShowGPU: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "OsdShowGPU", value: osdShowGPU) }
    }
    var osdShowResolution: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "OsdShowResolution", value: osdShowResolution) }
    }
    var osdShowGSStats: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "OsdShowGSStats", value: osdShowGSStats) }
    }
    var osdShowIndicators: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "OsdShowIndicators", value: osdShowIndicators) }
    }
    var osdShowSettings: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "OsdShowSettings", value: osdShowSettings) }
    }
    var osdShowInputs: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "OsdShowInputs", value: osdShowInputs) }
    }
    var osdShowFrameTimes: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "OsdShowFrameTimes", value: osdShowFrameTimes) }
    }
    var osdShowVersion: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "OsdShowVersion", value: osdShowVersion) }
    }
    var osdShowHardwareInfo: Bool {
        didSet { ARMSX2Bridge.setINIBool("EmuCore/GS", key: "OsdShowHardwareInfo", value: osdShowHardwareInfo) }
    }
    var osdShowDeviceStats: Bool {
        didSet { ARMSX2Bridge.setINIBool("ARMSX2iOS/UI", key: "OsdShowDeviceStats", value: osdShowDeviceStats) }
    }

    // ── Gamepad / UI ──
    var padOpacity: Float {
        didSet { ARMSX2Bridge.setINIFloat("ARMSX2iOS/UI", key: "PadOpacity", value: padOpacity) }
    }
    var hapticFeedback: Bool {
        didSet { ARMSX2Bridge.setINIBool("ARMSX2iOS/UI", key: "HapticFeedback", value: hapticFeedback) }
    }
    var virtualPadSkin: VirtualPadSkin {
        didSet { ARMSX2Bridge.setINIInt("ARMSX2iOS/UI", key: "VirtualPadSkin", value: Int32(virtualPadSkin.rawValue)) }
    }
    var appLanguage: AppLanguage {
        didSet { ARMSX2Bridge.setINIString("ARMSX2iOS/UI", key: "AppLanguage", value: appLanguage.rawValue) }
    }
    var controllerMultitapMode: Int {
        didSet { ARMSX2Bridge.setINIInt("ARMSX2iOS/Gamepad", key: "MultitapMode", value: Int32(controllerMultitapMode)) }
    }

    // DEV9 / Network
    var dev9HddEnabled: Bool {
        didSet {
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIBool("DEV9/Hdd", key: "HddEnable", value: dev9HddEnabled)
            ARMSX2Bridge.setINIString("DEV9/Hdd", key: "HddFile", value: dev9HddFile)
        }
    }
    var dev9HddFile: String {
        didSet {
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIString("DEV9/Hdd", key: "HddFile", value: dev9HddFile)
        }
    }
    var dev9EthernetEnabled: Bool {
        didSet {
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIBool("DEV9/Eth", key: "EthEnable", value: dev9EthernetEnabled)
            if dev9EthernetEnabled {
                ARMSX2Bridge.setINIString("DEV9/Eth", key: "EthApi", value: "Sockets")
                ARMSX2Bridge.setINIString("DEV9/Eth", key: "EthDevice", value: dev9EthDevice.isEmpty ? "Auto" : dev9EthDevice)
            }
        }
    }
    var dev9EthDevice: String {
        didSet {
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIString("DEV9/Eth", key: "EthApi", value: "Sockets")
            ARMSX2Bridge.setINIString("DEV9/Eth", key: "EthDevice", value: dev9EthDevice.isEmpty ? "Auto" : dev9EthDevice)
        }
    }
    var dev9InterceptDHCP: Bool {
        didSet {
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIBool("DEV9/Eth", key: "InterceptDHCP", value: dev9InterceptDHCP)
        }
    }
    var dev9EthLogDHCP: Bool {
        didSet {
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIBool("DEV9/Eth", key: "EthLogDHCP", value: dev9EthLogDHCP)
        }
    }
    var dev9EthLogDNS: Bool {
        didSet {
            guard !suppressINIWrites else { return }
            ARMSX2Bridge.setINIBool("DEV9/Eth", key: "EthLogDNS", value: dev9EthLogDNS)
        }
    }

    private static func aspectRatioName(for value: Int) -> String {
        switch value {
        case 0: return "Stretch"
        case 1: return "Auto 4:3/3:2"
        case 2: return "4:3"
        case 3: return "16:9"
        case 4: return "10:7"
        default: return "Auto 4:3/3:2"
        }
    }

    private static func aspectRatioValue(from name: String) -> Int {
        switch name {
        case "Stretch", "0": return 0
        case "Auto 4:3/3:2", "1": return 1
        case "4:3", "2": return 2
        case "16:9", "3": return 3
        case "10:7", "4": return 4
        default: return 1
        }
    }

    // ── Init from INI ──
    private init() {
        // CPU
        eeCoreType = Int(ARMSX2Bridge.getINIInt("EmuCore/CPU", key: "CoreType", defaultValue: 2))
        iopRecompiler = ARMSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "EnableIOP", defaultValue: true)
        vu0Recompiler = ARMSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "EnableVU0", defaultValue: true)
        vu1Recompiler = ARMSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "EnableVU1", defaultValue: true)
        fastBoot = ARMSX2Bridge.getINIBool("GameISO", key: "FastBoot", defaultValue: false)
        fastmem = ARMSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "EnableFastmem", defaultValue: true)
        let loadedNTSCFramerate = ARMSX2Bridge.getINIFloat("EmuCore/GS", key: "FramerateNTSC", defaultValue: 59.94)
        ntscFramerate = loadedNTSCFramerate
        palFramerate = ARMSX2Bridge.getINIFloat("EmuCore/GS", key: "FrameratePAL", defaultValue: 50.0)
        let nominalScalar = ARMSX2Bridge.getINIFloat("Framerate", key: "NominalScalar", defaultValue: 1.0)
        frameLimiterEnabled = Self.frameLimiterEnabled(fromNominalScalar: nominalScalar)
        targetFPS = Self.targetFPS(fromNominalScalar: nominalScalar, baseFramerate: loadedNTSCFramerate)
        Self.sanitizeNominalScalarIfNeeded(nominalScalar)
        // Boot
        fastCDVD = ARMSX2Bridge.getINIBool("EmuCore/Speedhacks", key: "fastCDVD", defaultValue: false)
        // Advanced Speedhacks
        eeCycleRate = Int(ARMSX2Bridge.getINIInt("EmuCore/Speedhacks", key: "EECycleRate", defaultValue: 0))
        vu1Instant = ARMSX2Bridge.getINIBool("EmuCore/Speedhacks", key: "vu1Instant", defaultValue: true)
        mtvu = ARMSX2Bridge.getINIBool("EmuCore/Speedhacks", key: "vuThread", defaultValue: false)
        waitLoop = ARMSX2Bridge.getINIBool("EmuCore/Speedhacks", key: "WaitLoop", defaultValue: true)
        intcStat = ARMSX2Bridge.getINIBool("EmuCore/Speedhacks", key: "IntcStat", defaultValue: true)
        enableCheats = ARMSX2Bridge.getINIBool("EmuCore", key: "EnableCheats", defaultValue: false)
        enablePatches = ARMSX2Bridge.getINIBool("EmuCore", key: "EnablePatches", defaultValue: true)
        enableGameFixes = ARMSX2Bridge.getINIBool("EmuCore", key: "EnableGameFixes", defaultValue: true)
        enableGameDBHardwareFixes = !ARMSX2Bridge.getINIBool("EmuCore/GS", key: "UserHacks", defaultValue: false)
        enableWidescreenPatches = ARMSX2Bridge.getINIBool("EmuCore", key: "EnableWideScreenPatches", defaultValue: false)
        enableNoInterlacingPatches = ARMSX2Bridge.getINIBool("EmuCore", key: "EnableNoInterlacingPatches", defaultValue: false)
        // Graphics
#if targetEnvironment(macCatalyst)
        renderer = 17
        ARMSX2Bridge.setINIInt("EmuCore/GS", key: "Renderer", value: Int32(17))
#else
        let initialRenderer = Self.supportedIOSRenderer(Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "Renderer", defaultValue: 17)))
        renderer = initialRenderer
        ARMSX2Bridge.setINIInt("EmuCore/GS", key: "Renderer", value: Int32(initialRenderer))
#endif
        upscaleMultiplier = ARMSX2Bridge.getINIFloat("EmuCore/GS", key: "upscale_multiplier", defaultValue: 1.0)
        vsyncQueueSize = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "VsyncQueueSize", defaultValue: 8))
        textureFiltering = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "filter", defaultValue: 2))
        hardwareMipmapping = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "hw_mipmap", defaultValue: true)
        fxaa = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "fxaa", defaultValue: false)
        casMode = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "CASMode", defaultValue: 0))
        casSharpness = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "CASSharpness", defaultValue: 50))
        interlaceMode = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "deinterlace_mode", defaultValue: 7))
        aspectRatio = Self.aspectRatioValue(from: ARMSX2Bridge.getINIString("EmuCore/GS", key: "AspectRatio", defaultValue: "Auto 4:3/3:2"))
        blendingAccuracy = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "accurate_blending_unit", defaultValue: 1))
        dithering = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "dithering_ps2", defaultValue: 2))
        loadTextureReplacements = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "LoadTextureReplacements", defaultValue: false)
        loadTextureReplacementsAsync = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "LoadTextureReplacementsAsync", defaultValue: true)
        precacheTextureReplacements = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "PrecacheTextureReplacements", defaultValue: false)
        texturePreloading = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "texture_preloading", defaultValue: 2))
        dumpReplaceableTextures = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "DumpReplaceableTextures", defaultValue: false)
        dumpReplaceableMipmaps = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "DumpReplaceableMipmaps", defaultValue: false)
        dumpTexturesWithFMVActive = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "DumpTexturesWithFMVActive", defaultValue: false)
        dumpDirectTextures = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "DumpDirectTextures", defaultValue: true)
        dumpPaletteTextures = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "DumpPaletteTextures", defaultValue: true)
        // OSD
        let loadedOsdPreset = OsdPreset(rawValue: Int(ARMSX2Bridge.getINIInt("ARMSX2iOS/UI", key: "OsdPreset", defaultValue: 0))) ?? .off
        osdPreset = loadedOsdPreset
        osdPerformancePosition = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "OsdPerformancePos", defaultValue: 2))
        osdShowFPS = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowFPS", defaultValue: false)
        osdShowVPS = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowVPS", defaultValue: false)
        osdShowSpeed = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowSpeed", defaultValue: false)
        osdShowCPU = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowCPU", defaultValue: false)
        osdShowGPU = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowGPU", defaultValue: false)
        osdShowResolution = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowResolution", defaultValue: false)
        osdShowGSStats = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowGSStats", defaultValue: false)
        osdShowIndicators = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowIndicators", defaultValue: false)
        osdShowSettings = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowSettings", defaultValue: false)
        osdShowInputs = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowInputs", defaultValue: false)
        osdShowFrameTimes = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowFrameTimes", defaultValue: false)
        osdShowVersion = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowVersion", defaultValue: false)
        osdShowHardwareInfo = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowHardwareInfo", defaultValue: false)
        osdShowDeviceStats = ARMSX2Bridge.getINIBool("ARMSX2iOS/UI", key: "OsdShowDeviceStats", defaultValue: loadedOsdPreset != .off)
        // UI
        padOpacity = ARMSX2Bridge.getINIFloat("ARMSX2iOS/UI", key: "PadOpacity", defaultValue: 0.6)
        hapticFeedback = ARMSX2Bridge.getINIBool("ARMSX2iOS/UI", key: "HapticFeedback", defaultValue: true)
        virtualPadSkin = VirtualPadSkin(rawValue: Int(ARMSX2Bridge.getINIInt("ARMSX2iOS/UI", key: "VirtualPadSkin", defaultValue: 0))) ?? .armsx2Refresh
        appLanguage = AppLanguage(rawValue: ARMSX2Bridge.getINIString("ARMSX2iOS/UI", key: "AppLanguage", defaultValue: AppLanguage.system.rawValue)) ?? .system
        controllerMultitapMode = Int(ARMSX2Bridge.getINIInt("ARMSX2iOS/Gamepad", key: "MultitapMode", defaultValue: 0))
        dev9HddEnabled = ARMSX2Bridge.getINIBool("DEV9/Hdd", key: "HddEnable", defaultValue: false)
        dev9HddFile = ARMSX2Bridge.getINIString("DEV9/Hdd", key: "HddFile", defaultValue: "DEV9hdd.raw")
        dev9EthernetEnabled = ARMSX2Bridge.getINIBool("DEV9/Eth", key: "EthEnable", defaultValue: false)
        dev9EthDevice = ARMSX2Bridge.getINIString("DEV9/Eth", key: "EthDevice", defaultValue: "Auto")
        dev9InterceptDHCP = ARMSX2Bridge.getINIBool("DEV9/Eth", key: "InterceptDHCP", defaultValue: false)
        dev9EthLogDHCP = ARMSX2Bridge.getINIBool("DEV9/Eth", key: "EthLogDHCP", defaultValue: false)
        dev9EthLogDNS = ARMSX2Bridge.getINIBool("DEV9/Eth", key: "EthLogDNS", defaultValue: false)
        ARMSX2Bridge.setINIString("EmuCore/GS", key: "AspectRatio", value: Self.aspectRatioName(for: aspectRatio))
        // Apply OSD preset
        ARMSX2Bridge.applyOsdPreset(Int32(osdPreset.rawValue))
    }

    /// Reload ALL settings from INI (call on VM start/stop)
    func reload() {
        suppressINIWrites = true
        defer { suppressINIWrites = false }

        eeCoreType = Int(ARMSX2Bridge.getINIInt("EmuCore/CPU", key: "CoreType", defaultValue: 2))
        iopRecompiler = ARMSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "EnableIOP", defaultValue: true)
        vu0Recompiler = ARMSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "EnableVU0", defaultValue: true)
        vu1Recompiler = ARMSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "EnableVU1", defaultValue: true)
        fastBoot = ARMSX2Bridge.getINIBool("GameISO", key: "FastBoot", defaultValue: false)
        fastmem = ARMSX2Bridge.getINIBool("EmuCore/CPU/Recompiler", key: "EnableFastmem", defaultValue: true)
        ntscFramerate = ARMSX2Bridge.getINIFloat("EmuCore/GS", key: "FramerateNTSC", defaultValue: 59.94)
        palFramerate = ARMSX2Bridge.getINIFloat("EmuCore/GS", key: "FrameratePAL", defaultValue: 50.0)
        let nominalScalar = ARMSX2Bridge.getINIFloat("Framerate", key: "NominalScalar", defaultValue: 1.0)
        frameLimiterEnabled = Self.frameLimiterEnabled(fromNominalScalar: nominalScalar)
        targetFPS = Self.targetFPS(fromNominalScalar: nominalScalar, baseFramerate: ntscFramerate)
        Self.sanitizeNominalScalarIfNeeded(nominalScalar)
        fastCDVD = ARMSX2Bridge.getINIBool("EmuCore/Speedhacks", key: "fastCDVD", defaultValue: false)
        eeCycleRate = Int(ARMSX2Bridge.getINIInt("EmuCore/Speedhacks", key: "EECycleRate", defaultValue: 0))
        vu1Instant = ARMSX2Bridge.getINIBool("EmuCore/Speedhacks", key: "vu1Instant", defaultValue: true)
        mtvu = ARMSX2Bridge.getINIBool("EmuCore/Speedhacks", key: "vuThread", defaultValue: false)
        waitLoop = ARMSX2Bridge.getINIBool("EmuCore/Speedhacks", key: "WaitLoop", defaultValue: true)
        intcStat = ARMSX2Bridge.getINIBool("EmuCore/Speedhacks", key: "IntcStat", defaultValue: true)
        enableCheats = ARMSX2Bridge.getINIBool("EmuCore", key: "EnableCheats", defaultValue: false)
        enablePatches = ARMSX2Bridge.getINIBool("EmuCore", key: "EnablePatches", defaultValue: true)
        enableGameFixes = ARMSX2Bridge.getINIBool("EmuCore", key: "EnableGameFixes", defaultValue: true)
        enableGameDBHardwareFixes = !ARMSX2Bridge.getINIBool("EmuCore/GS", key: "UserHacks", defaultValue: false)
        enableWidescreenPatches = ARMSX2Bridge.getINIBool("EmuCore", key: "EnableWideScreenPatches", defaultValue: false)
        enableNoInterlacingPatches = ARMSX2Bridge.getINIBool("EmuCore", key: "EnableNoInterlacingPatches", defaultValue: false)
#if targetEnvironment(macCatalyst)
        renderer = 17
        ARMSX2Bridge.setINIInt("EmuCore/GS", key: "Renderer", value: Int32(17))
#else
        renderer = Self.supportedIOSRenderer(Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "Renderer", defaultValue: 17)))
        ARMSX2Bridge.setINIInt("EmuCore/GS", key: "Renderer", value: Int32(renderer))
#endif
        upscaleMultiplier = ARMSX2Bridge.getINIFloat("EmuCore/GS", key: "upscale_multiplier", defaultValue: 1.0)
        vsyncQueueSize = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "VsyncQueueSize", defaultValue: 8))
        textureFiltering = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "filter", defaultValue: 2))
        hardwareMipmapping = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "hw_mipmap", defaultValue: true)
        fxaa = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "fxaa", defaultValue: false)
        casMode = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "CASMode", defaultValue: 0))
        casSharpness = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "CASSharpness", defaultValue: 50))
        interlaceMode = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "deinterlace_mode", defaultValue: 7))
        aspectRatio = Self.aspectRatioValue(from: ARMSX2Bridge.getINIString("EmuCore/GS", key: "AspectRatio", defaultValue: "Auto 4:3/3:2"))
        blendingAccuracy = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "accurate_blending_unit", defaultValue: 1))
        dithering = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "dithering_ps2", defaultValue: 2))
        loadTextureReplacements = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "LoadTextureReplacements", defaultValue: false)
        loadTextureReplacementsAsync = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "LoadTextureReplacementsAsync", defaultValue: true)
        precacheTextureReplacements = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "PrecacheTextureReplacements", defaultValue: false)
        texturePreloading = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "texture_preloading", defaultValue: 2))
        dumpReplaceableTextures = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "DumpReplaceableTextures", defaultValue: false)
        dumpReplaceableMipmaps = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "DumpReplaceableMipmaps", defaultValue: false)
        dumpTexturesWithFMVActive = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "DumpTexturesWithFMVActive", defaultValue: false)
        dumpDirectTextures = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "DumpDirectTextures", defaultValue: true)
        dumpPaletteTextures = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "DumpPaletteTextures", defaultValue: true)
        osdPreset = OsdPreset(rawValue: Int(ARMSX2Bridge.getINIInt("ARMSX2iOS/UI", key: "OsdPreset", defaultValue: 0))) ?? .off
        osdPerformancePosition = Int(ARMSX2Bridge.getINIInt("EmuCore/GS", key: "OsdPerformancePos", defaultValue: 2))
        osdShowFPS = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowFPS", defaultValue: false)
        osdShowVPS = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowVPS", defaultValue: false)
        osdShowSpeed = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowSpeed", defaultValue: false)
        osdShowCPU = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowCPU", defaultValue: false)
        osdShowGPU = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowGPU", defaultValue: false)
        osdShowResolution = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowResolution", defaultValue: false)
        osdShowGSStats = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowGSStats", defaultValue: false)
        osdShowIndicators = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowIndicators", defaultValue: false)
        osdShowSettings = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowSettings", defaultValue: false)
        osdShowInputs = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowInputs", defaultValue: false)
        osdShowFrameTimes = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowFrameTimes", defaultValue: false)
        osdShowVersion = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowVersion", defaultValue: false)
        osdShowHardwareInfo = ARMSX2Bridge.getINIBool("EmuCore/GS", key: "OsdShowHardwareInfo", defaultValue: false)
        osdShowDeviceStats = ARMSX2Bridge.getINIBool("ARMSX2iOS/UI", key: "OsdShowDeviceStats", defaultValue: osdPreset != .off)
        padOpacity = ARMSX2Bridge.getINIFloat("ARMSX2iOS/UI", key: "PadOpacity", defaultValue: 0.6)
        hapticFeedback = ARMSX2Bridge.getINIBool("ARMSX2iOS/UI", key: "HapticFeedback", defaultValue: true)
        virtualPadSkin = VirtualPadSkin(rawValue: Int(ARMSX2Bridge.getINIInt("ARMSX2iOS/UI", key: "VirtualPadSkin", defaultValue: 0))) ?? .armsx2Refresh
        appLanguage = AppLanguage(rawValue: ARMSX2Bridge.getINIString("ARMSX2iOS/UI", key: "AppLanguage", defaultValue: AppLanguage.system.rawValue)) ?? .system
        controllerMultitapMode = Int(ARMSX2Bridge.getINIInt("ARMSX2iOS/Gamepad", key: "MultitapMode", defaultValue: 0))
        dev9HddEnabled = ARMSX2Bridge.getINIBool("DEV9/Hdd", key: "HddEnable", defaultValue: false)
        dev9HddFile = ARMSX2Bridge.getINIString("DEV9/Hdd", key: "HddFile", defaultValue: "DEV9hdd.raw")
        dev9EthernetEnabled = ARMSX2Bridge.getINIBool("DEV9/Eth", key: "EthEnable", defaultValue: false)
        dev9EthDevice = ARMSX2Bridge.getINIString("DEV9/Eth", key: "EthDevice", defaultValue: "Auto")
        dev9InterceptDHCP = ARMSX2Bridge.getINIBool("DEV9/Eth", key: "InterceptDHCP", defaultValue: false)
        dev9EthLogDHCP = ARMSX2Bridge.getINIBool("DEV9/Eth", key: "EthLogDHCP", defaultValue: false)
        dev9EthLogDNS = ARMSX2Bridge.getINIBool("DEV9/Eth", key: "EthLogDNS", defaultValue: false)
    }

    private static func frameLimiterEnabled(fromNominalScalar scalar: Float) -> Bool {
        scalar < 5.0
    }

    private static func sanitizedNominalScalar(_ scalar: Float) -> Float {
        guard scalar.isFinite else { return 1.0 }
        return min(max(scalar, 0.05), 10.0)
    }

    private static func clampedTargetFPS(_ fps: Float) -> Float {
        guard fps.isFinite else { return defaultTargetFPS }
        return min(max(fps.rounded(), minTargetFPS), maxTargetFPS)
    }

    private static func targetFPS(fromNominalScalar scalar: Float, baseFramerate: Float) -> Float {
        guard frameLimiterEnabled(fromNominalScalar: scalar) else { return defaultTargetFPS }
        return clampedTargetFPS(sanitizedNominalScalar(scalar) * max(baseFramerate, 1.0))
    }

    private static func sanitizeNominalScalarIfNeeded(_ scalar: Float) {
        let sanitized = sanitizedNominalScalar(scalar)
        guard abs(scalar - sanitized) > 0.001 else { return }

        NSLog("[ARMSX2 iOS Settings] Clamping unsupported NominalScalar %.3f -> %.3f", scalar, sanitized)
        ARMSX2Bridge.setINIFloat("Framerate", key: "NominalScalar", value: sanitized)
    }

    private func applyFrameLimiterSettings() {
        guard !suppressINIWrites else { return }
        let scalar: Float = frameLimiterEnabled ? Self.sanitizedNominalScalar(targetFPS / max(ntscFramerate, 1.0)) : 10.0
        NSLog("[ARMSX2 iOS Settings] Frame limiter %@ targetFPS=%.0f NominalScalar=%.3f",
              frameLimiterEnabled ? "ON" : "OFF", targetFPS, scalar)
        ARMSX2Bridge.setINIFloat("Framerate", key: "NominalScalar", value: scalar)
    }

    private static func supportedIOSRenderer(_ value: Int) -> Int {
        switch value {
        case 17, 13, 11:
            return value
        default:
            return 17
        }
    }

    func localized(_ key: String) -> String {
        appLanguage.localized(key)
    }

    var localizedLayoutDirection: LayoutDirection {
        appLanguage.layoutDirection
    }

    /// Apply OSD preset — writes ALL OSD flags to INI + GSConfig
    private func applyOsdPreset(_ preset: OsdPreset) {
        ARMSX2Bridge.applyOsdPreset(Int32(preset.rawValue))
        if preset == .off {
            osdPerformancePosition = 0
        } else if osdPerformancePosition == 0 {
            osdPerformancePosition = 2
        }
        let isSimple = preset == .simple
        let isDetail = preset == .detail
        let isFull = preset == .full
        osdShowFPS = isSimple || isDetail || isFull
        osdShowVPS = isDetail || isFull
        osdShowSpeed = isSimple || isDetail || isFull
        osdShowCPU = isSimple || isDetail || isFull
        osdShowGPU = isSimple || isDetail || isFull
        osdShowResolution = isDetail || isFull
        osdShowGSStats = isFull
        osdShowIndicators = isSimple || isDetail || isFull
        osdShowSettings = isFull
        osdShowInputs = isFull
        osdShowFrameTimes = isFull
        osdShowVersion = isFull
        osdShowHardwareInfo = isFull
        osdShowDeviceStats = isSimple || isDetail || isFull
    }

    /// Reset emulator settings to ARMSX2 iOS defaults
    func resetEmulatorDefaults() {
        eeCoreType = 2          // ARM64 JIT
        iopRecompiler = true
        vu0Recompiler = true
        vu1Recompiler = true
        fastBoot = false
        fastmem = true
        targetFPS = Self.defaultTargetFPS
        frameLimiterEnabled = true
        ntscFramerate = 59.94
        palFramerate = 50.0
        fastCDVD = false
        eeCycleRate = 0
        vu1Instant = true
        mtvu = false
        waitLoop = true
        intcStat = true
        enableCheats = false
        enablePatches = true
        enableGameFixes = true
        enableGameDBHardwareFixes = true
        enableWidescreenPatches = false
        enableNoInterlacingPatches = false
    }

    /// Keep EE/IOP/VU0 fast while isolating suspected VU1 JIT regressions.
    func applyVU1CompatibilityPreset() {
        eeCoreType = 2
        iopRecompiler = true
        vu0Recompiler = true
        vu1Recompiler = false
        vu1Instant = false
        mtvu = false
        fastmem = false
    }

    /// Slow diagnostic preset for crash isolation when dynarec state is suspect.
    func applyFullInterpreterPreset() {
        eeCoreType = 1
        iopRecompiler = false
        vu0Recompiler = false
        vu1Recompiler = false
        vu1Instant = false
        mtvu = false
        fastmem = false
    }

    /// Reset graphics settings to ARMSX2 iOS defaults
    func resetGraphicsDefaults() {
        renderer = 17           // Metal
        upscaleMultiplier = 1.0 // Native PS2
        vsyncQueueSize = 8
        textureFiltering = 2    // Bilinear (PS2)
        hardwareMipmapping = true
        fxaa = false
        casMode = 0             // Disabled
        casSharpness = 50
        interlaceMode = 7       // Adaptive
        aspectRatio = 1         // Auto 4:3/3:2
        blendingAccuracy = 1    // Basic
        dithering = 2           // Scaled
        // Texture pack and dump toggles are intentionally preserved.
    }
}
