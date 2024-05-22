#include <android/log.h>
#include "Hook.h"
#include "Plugin.h"
#include "Log.h"
#include "../deps/UnityResolve/UnityResolve.hpp"
#include "Il2cppUtils.hpp"
#include "Local.h"
#include <unordered_set>
#include "camera/camera.hpp"
#include "config/Config.hpp"
#include "shadowhook.h"
#include <jni.h>
#include <thread>


std::unordered_set<void*> hookedStubs{};

#define DEFINE_HOOK(returnType, name, params)                                                      \
	using name##_Type = returnType(*) params;                                                      \
	name##_Type name##_Addr = nullptr;                                                             \
	name##_Type name##_Orig = nullptr;                                                             \
	returnType name##_Hook params


#define ADD_HOOK(name, addr)                                                                       \
	name##_Addr = reinterpret_cast<name##_Type>(addr);                                             \
	if (addr) {                                                                                    \
    	auto stub = hookInstaller->InstallHook(reinterpret_cast<void*>(addr),                      \
                                               reinterpret_cast<void*>(name##_Hook),               \
                                               reinterpret_cast<void**>(&name##_Orig));            \
        if (stub == NULL) {                                                                        \
            int error_num = shadowhook_get_errno();                                                \
            const char *error_msg = shadowhook_to_errmsg(error_num);                               \
            Log::ErrorFmt("ADD_HOOK: %s at %p failed: %s", #name, addr, error_msg);                \
        }                                                                                          \
        else {                                                                                     \
            hookedStubs.emplace(stub);                                                             \
            GakumasLocal::Log::InfoFmt("ADD_HOOK: %s at %p", #name, addr);                         \
        }                                                                                          \
    }                                                                                              \
    else GakumasLocal::Log::ErrorFmt("Hook failed: %s is NULL", #name, addr)

void UnHookAll() {
    for (const auto i: hookedStubs) {
        int result = shadowhook_unhook(i);
        if(result != 0)
        {
            int error_num = shadowhook_get_errno();
            const char *error_msg = shadowhook_to_errmsg(error_num);
            GakumasLocal::Log::ErrorFmt("unhook failed: %d - %s", error_num, error_msg);
        }
    }
}

namespace GakumasLocal::HookMain {
    using Il2cppString = UnityResolve::UnityType::String;

    UnityResolve::UnityType::String* environment_get_stacktrace() {
        /*
        static auto mtd = Il2cppUtils::GetMethod("mscorlib.dll", "System",
                                                 "Environment", "get_StackTrace");
        return mtd->Invoke<UnityResolve::UnityType::String*>();*/
        const auto pClass = Il2cppUtils::GetClass("mscorlib.dll", "System.Diagnostics",
                                                  "StackTrace");

        const auto ctor_mtd = Il2cppUtils::GetMethod("mscorlib.dll", "System.Diagnostics",
                                                     "StackTrace", ".ctor");
        const auto toString_mtd = Il2cppUtils::GetMethod("mscorlib.dll", "System.Diagnostics",
                                                         "StackTrace", "ToString");

        const auto klassInstance = pClass->New<void*>();
        ctor_mtd->Invoke<void>(klassInstance);
        return toString_mtd->Invoke<Il2cppString*>(klassInstance);
    }

    DEFINE_HOOK(void, Internal_LogException, (void* ex, void* obj)) {
        Internal_LogException_Orig(ex, obj);
        // Log::LogFmt(ANDROID_LOG_VERBOSE, "UnityLog - Internal_LogException");
    }

    DEFINE_HOOK(void, Internal_Log, (int logType, int logOption, UnityResolve::UnityType::String* content, void* context)) {
        Internal_Log_Orig(logType, logOption, content, context);
        // 2022.3.21f1
        // Log::LogFmt(ANDROID_LOG_VERBOSE, "UnityLog - Internal_Log: %s", content->ToString().c_str());
    }

    UnityResolve::UnityType::Camera* mainCameraCache = nullptr;
    UnityResolve::UnityType::Transform* cameraTransformCache = nullptr;
    void CheckAndUpdateMainCamera() {
        if (!Config::enableFreeCamera) return;
        static auto IsNativeObjectAlive = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll", "UnityEngine",
                                                                 "Object", "IsNativeObjectAlive");

        if (IsNativeObjectAlive->Invoke<bool>(mainCameraCache)) return;

        mainCameraCache = UnityResolve::UnityType::Camera::GetMain();
        cameraTransformCache = mainCameraCache->GetTransform();
    }

    DEFINE_HOOK(void, Unity_set_position_Injected, (UnityResolve::UnityType::Transform* _this, UnityResolve::UnityType::Vector3* data)) {
        if (Config::enableFreeCamera) {
            CheckAndUpdateMainCamera();

            if (cameraTransformCache == _this) {
                //Log::DebugFmt("MainCamera set pos: %f, %f, %f", data->x, data->y, data->z);
                auto& origCameraPos = GKCamera::baseCamera.pos;
                data->x = origCameraPos.x;
                data->y = origCameraPos.y;
                data->z = origCameraPos.z;
            }
        }

        return Unity_set_position_Injected_Orig(_this, data);
    }

    DEFINE_HOOK(void, Unity_set_rotation_Injected, (UnityResolve::UnityType::Transform* _this, UnityResolve::UnityType::Quaternion* value)) {
        if (Config::enableFreeCamera) {
            if (cameraTransformCache == _this) {
                auto& origCameraLookat = GKCamera::baseCamera.lookAt;
                static auto lookat_injected = reinterpret_cast<void (*)(void*_this,
                                                                        UnityResolve::UnityType::Vector3* worldPosition, UnityResolve::UnityType::Vector3* worldUp)>(
                        Il2cppUtils::il2cpp_resolve_icall(
                                "UnityEngine.Transform::Internal_LookAt_Injected(UnityEngine.Vector3&,UnityEngine.Vector3&)"));
                static auto worldUp = UnityResolve::UnityType::Vector3(0, 1, 0);
                lookat_injected(_this, &origCameraLookat, &worldUp);
                // TODO 相机 FOV
                return;
            }
        }
        return Unity_set_rotation_Injected_Orig(_this, value);
    }

    std::unordered_map<void*, std::string> loadHistory{};

    DEFINE_HOOK(void*, AssetBundle_LoadAssetAsync, (void* _this, Il2cppString* name, void* type)) {
        // Log::InfoFmt("AssetBundle_LoadAssetAsync: %s, type: %s", name->ToString().c_str());
        auto ret = AssetBundle_LoadAssetAsync_Orig(_this, name, type);
        loadHistory.emplace(ret, name->ToString());
        return ret;
    }

    DEFINE_HOOK(void*, AssetBundleRequest_GetResult, (void* _this)) {
        auto result = AssetBundleRequest_GetResult_Orig(_this);
        if (const auto iter = loadHistory.find(_this); iter != loadHistory.end()) {
            const auto name = iter->second;
            loadHistory.erase(iter);

            // const auto assetClass = Il2cppUtils::get_class_from_instance(result);
            // Log::InfoFmt("AssetBundleRequest_GetResult: %s, type: %s", name.c_str(), static_cast<Il2CppClassHead*>(assetClass)->name);
        }
        return result;
    }

    DEFINE_HOOK(void*, Resources_Load, (Il2cppString* path, void* systemTypeInstance)) {
        auto ret = Resources_Load_Orig(path, systemTypeInstance);

        // if (ret) Log::DebugFmt("Resources_Load: %s, type: %s", path->ToString().c_str(), Il2cppUtils::get_class_from_instance(ret)->name);

        return ret;
    }

    DEFINE_HOOK(void, I18nHelper_SetUpI18n, (void* _this, Il2cppString* lang, Il2cppString* localizationText, int keyComparison)) {
        // Log::InfoFmt("SetUpI18n lang: %s, key: %d text: %s", lang->ToString().c_str(), keyComparison, localizationText->ToString().c_str());
        // TODO 此处为 dump 原文 csv
        I18nHelper_SetUpI18n_Orig(_this, lang, localizationText, keyComparison);
    }

    DEFINE_HOOK(void, I18nHelper_SetValue, (void* _this, Il2cppString* key, Il2cppString* value)) {
        // Log::InfoFmt("I18nHelper_SetValue: %s - %s", key->ToString().c_str(), value->ToString().c_str());
        std::string local;
        if (Local::GetI18n(key->ToString(), &local)) {
            I18nHelper_SetValue_Orig(_this, key, UnityResolve::UnityType::String::New(local));
            return;
        }
        Local::DumpI18nItem(key->ToString(), value->ToString());
        I18nHelper_SetValue_Orig(_this, key, value);
    }

    void* fontCache = nullptr;
    void* GetReplaceFont() {
        static auto CreateFontFromPath = reinterpret_cast<void (*)(void* self, Il2cppString* path)>(
                Il2cppUtils::il2cpp_resolve_icall("UnityEngine.Font::Internal_CreateFontFromPath(UnityEngine.Font,System.String)")
        );
        static auto Font_klass = Il2cppUtils::GetClass("UnityEngine.TextRenderingModule.dll",
                                                       "UnityEngine", "Font");
        static auto Font_ctor = Il2cppUtils::GetMethod("UnityEngine.TextRenderingModule.dll",
                                                       "UnityEngine", "Font", ".ctor");
        static auto IsNativeObjectAlive = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll", "UnityEngine",
                                                                 "Object", "IsNativeObjectAlive");
        if (fontCache) {
            if (IsNativeObjectAlive->Invoke<bool>(fontCache)) {
                return fontCache;
            }
        }

        const auto newFont = Font_klass->New<void*>();
        Font_ctor->Invoke<void>(newFont);

        static std::string fontName = Local::GetBasePath() / "local-files" / "MO-UDShinGo-SC-Gb4-M.otf";
        CreateFontFromPath(newFont, Il2cppString::New(fontName));
        fontCache = newFont;
        return newFont;
    }

    std::unordered_set<void*> updatedFontPtrs{};
    void UpdateFont(void* TMP_Text_this) {
        static auto get_font = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll",
                                                      "TMPro", "TMP_Text", "get_font");
        static auto set_font = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll",
                                                      "TMPro", "TMP_Text", "set_font");

        static auto set_sourceFontFile = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll", "TMPro",
                                                                "TMP_FontAsset", "set_sourceFontFile");
        static auto UpdateFontAssetData = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll", "TMPro",
                                                                 "TMP_FontAsset", "UpdateFontAssetData");

        auto fontAsset = get_font->Invoke<void*>(TMP_Text_this);
        auto newFont = GetReplaceFont();
        if (fontAsset && newFont) {
            set_sourceFontFile->Invoke<void>(fontAsset, newFont);
            if (!updatedFontPtrs.contains(fontAsset)) {
                updatedFontPtrs.emplace(fontAsset);
                UpdateFontAssetData->Invoke<void>(fontAsset);
            }
        }
        set_font->Invoke<void>(TMP_Text_this, fontAsset);
    }

    DEFINE_HOOK(void, TMP_Text_set_text, (void* _this, UnityResolve::UnityType::String* text)) {
        // Log::DebugFmt("TMP_Text_set_text: %s", text->ToString().c_str());
        std::string transText;
        if (Local::GetGenericText(text->ToString(), &transText)) {
            return TMP_Text_set_text_Orig(_this, UnityResolve::UnityType::String::New(transText));
        }

        // TMP_Text_set_text_Orig(_this, UnityResolve::UnityType::String::New("[TS]" + text->ToString()));
        TMP_Text_set_text_Orig(_this, text);

        static auto set_font = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll",
                                                      "TMPro", "TMP_Text", "set_font");
        //auto font = GetReplaceFontAsset();
        //set_font->Invoke<void>(_this, font);
        UpdateFont(_this);
    }

    DEFINE_HOOK(void, TextMeshProUGUI_Awake, (void* _this, void* method)) {
        // Log::InfoFmt("TextMeshProUGUI_Awake at %p, _this at %p", TextMeshProUGUI_Awake_Orig, _this);

        const auto TMP_Text_klass = Il2cppUtils::GetClass("Unity.TextMeshPro.dll",
                                                                     "TMPro", "TMP_Text");
        const auto get_Text_method = TMP_Text_klass->Get<UnityResolve::Method>("get_text");
        const auto set_Text_method = TMP_Text_klass->Get<UnityResolve::Method>("set_text");
        const auto currText = get_Text_method->Invoke<UnityResolve::UnityType::String*>(_this);
        if (currText) {
            //Log::InfoFmt("TextMeshProUGUI_Awake: %s", currText->ToString().c_str());
            std::string transText;
            if (Local::GetGenericText(currText->ToString(), &transText)) {
                TMP_Text_set_text_Orig(_this, UnityResolve::UnityType::String::New(transText));
            }
        }

        // set_font->Invoke<void>(_this, font);
        UpdateFont(_this);
        TextMeshProUGUI_Awake_Orig(_this, method);
    }

    // TODO 文本未hook完整 思路：从tips下手...
    DEFINE_HOOK(void, TextField_set_value, (void* _this, Il2cppString* value)) {
        Log::DebugFmt("TextField_set_value: %s", value->ToString().c_str());
        TextField_set_value_Orig(_this, value);
    }

    DEFINE_HOOK(Il2cppString*, OctoCaching_GetResourceFileName, (void* data, void* method)) {
        auto ret = OctoCaching_GetResourceFileName_Orig(data, method);
        //Log::DebugFmt("OctoCaching_GetResourceFileName: %s", ret->ToString().c_str());

        return ret;
    }

    DEFINE_HOOK(void, OctoResourceLoader_LoadFromCacheOrDownload,
                (void* _this, Il2cppString* resourceName, void* onComplete, void* onProgress, void* method)) {

        Log::DebugFmt("OctoResourceLoader_LoadFromCacheOrDownload: %s\n", resourceName->ToString().c_str());

        std::string replaceStr;
        if (Local::GetResourceText(resourceName->ToString(), &replaceStr)) {
            const auto onComplete_klass = Il2cppUtils::get_class_from_instance(onComplete);
            const auto onComplete_invoke_mtd = UnityResolve::Invoke<Il2cppUtils::MethodInfo*>(
                    "il2cpp_class_get_method_from_name", onComplete_klass, "Invoke", 2);
            if (onComplete_invoke_mtd) {
                const auto onComplete_invoke = reinterpret_cast<void (*)(void*, Il2cppString*, void*)>(
                        onComplete_invoke_mtd->methodPointer
                );
                onComplete_invoke(onComplete, UnityResolve::UnityType::String::New(replaceStr), NULL);
                return;
            }
        }

        return OctoResourceLoader_LoadFromCacheOrDownload_Orig(_this, resourceName, onComplete, onProgress, method);
    }

    DEFINE_HOOK(void, OnDownloadProgress_Invoke, (void* _this, Il2cppString* name, uint64_t receivedLength, uint64_t contentLength)) {
        Log::DebugFmt("OnDownloadProgress_Invoke: %s, %lu/%lu", name->ToString().c_str(), receivedLength, contentLength);
        OnDownloadProgress_Invoke_Orig(_this, name, receivedLength, contentLength);
    }

    // UnHooked
    DEFINE_HOOK(UnityResolve::UnityType::String*, UI_I18n_GetOrDefault, (void* _this,
            UnityResolve::UnityType::String* key, UnityResolve::UnityType::String* defaultKey, void* method)) {

        auto ret = UI_I18n_GetOrDefault_Orig(_this, key, defaultKey, method);

        // Log::DebugFmt("UI_I18n_GetOrDefault: key: %s, default: %s, result: %s", key->ToString().c_str(), defaultKey->ToString().c_str(), ret->ToString().c_str());

        return ret;
        // return UnityResolve::UnityType::String::New("[I18]" + ret->ToString());
    }

    void StartInjectFunctions() {
        const auto hookInstaller = Plugin::GetInstance().GetHookInstaller();
        UnityResolve::Init(xdl_open(hookInstaller->m_il2cppLibraryPath.c_str(), RTLD_NOW), UnityResolve::Mode::Il2Cpp);

        ADD_HOOK(AssetBundle_LoadAssetAsync, Il2cppUtils::il2cpp_resolve_icall(
                "UnityEngine.AssetBundle::LoadAssetAsync_Internal(System.String,System.Type)"));
        ADD_HOOK(AssetBundleRequest_GetResult, Il2cppUtils::il2cpp_resolve_icall(
                "UnityEngine.AssetBundleRequest::GetResult()"));
        ADD_HOOK(Resources_Load, Il2cppUtils::il2cpp_resolve_icall(
                "UnityEngine.ResourcesAPIInternal::Load(System.String,System.Type)"));

        ADD_HOOK(I18nHelper_SetUpI18n, Il2cppUtils::GetMethodPointer("quaunity-ui.Runtime.dll", "Qua.UI",
                                                                     "I18nHelper", "SetUpI18n"));
        ADD_HOOK(I18nHelper_SetValue, Il2cppUtils::GetMethodPointer("quaunity-ui.Runtime.dll", "Qua.UI",
                                                                     "I18n", "SetValue"));

        //ADD_HOOK(UI_I18n_GetOrDefault, Il2cppUtils::GetMethodPointer("quaunity-ui.Runtime.dll", "Qua.UI",
        //                                                             "I18n", "GetOrDefault"));

        ADD_HOOK(TextMeshProUGUI_Awake, Il2cppUtils::GetMethodPointer("Unity.TextMeshPro.dll", "TMPro",
                                                                      "TextMeshProUGUI", "Awake"));

        ADD_HOOK(TMP_Text_set_text, Il2cppUtils::GetMethodPointer("Unity.TextMeshPro.dll", "TMPro",
                                                                      "TMP_Text", "set_text"));

        ADD_HOOK(TextField_set_value, Il2cppUtils::GetMethodPointer("UnityEngine.UIElementsModule.dll", "UnityEngine.UIElements",
                                                                  "TextField", "set_value"));

        ADD_HOOK(OctoCaching_GetResourceFileName, Il2cppUtils::GetMethodPointer("Octo.dll", "Octo.Caching",
                                                                     "OctoCaching", "GetResourceFileName"));

        ADD_HOOK(OctoResourceLoader_LoadFromCacheOrDownload,
                 Il2cppUtils::GetMethodPointer("Octo.dll", "Octo.Loader",
                                               "OctoResourceLoader", "LoadFromCacheOrDownload",
                                               {"System.String", "System.Action<System.String,Octo.LoadError>", "Octo.OnDownloadProgress"}));

        ADD_HOOK(OnDownloadProgress_Invoke,
                 Il2cppUtils::GetMethodPointer("Octo.dll", "Octo",
                                               "OnDownloadProgress", "Invoke"));

        ADD_HOOK(Internal_LogException, Il2cppUtils::il2cpp_resolve_icall(
                "UnityEngine.DebugLogHandler::Internal_LogException(System.Exception,UnityEngine.Object)"));
        ADD_HOOK(Internal_Log, Il2cppUtils::il2cpp_resolve_icall(
                "UnityEngine.DebugLogHandler::Internal_Log(UnityEngine.LogType,UnityEngine.LogOption,System.String,UnityEngine.Object)"));

        ADD_HOOK(Unity_set_position_Injected, Il2cppUtils::il2cpp_resolve_icall(
                "UnityEngine.Transform::set_position_Injected(UnityEngine.Vector3&)"));
        ADD_HOOK(Unity_set_rotation_Injected, Il2cppUtils::il2cpp_resolve_icall(
                "UnityEngine.Transform::set_rotation_Injected(UnityEngine.Quaternion&)"));
    }
    // 77 2640 5000

    DEFINE_HOOK(int, il2cpp_init, (const char* domain_name)) {
        const auto ret = il2cpp_init_Orig(domain_name);
        // InjectFunctions();

        Log::Info("Waiting for config...");

        while (!Config::isConfigInit) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!Config::enabled) {
            Log::Info("Plugin not enabled");
            return ret;
        }

        Log::Info("Start init plugin...");

        StartInjectFunctions();
        GKCamera::initCameraSettings();
        Local::LoadData();

        Log::Info("Plugin init finished.");
        return ret;
    }
}


namespace GakumasLocal::Hook {
    void Install() {
        const auto hookInstaller = Plugin::GetInstance().GetHookInstaller();

        Log::Info("Installing hook");

        ADD_HOOK(HookMain::il2cpp_init,
                  Plugin::GetInstance().GetHookInstaller()->LookupSymbol("il2cpp_init"));

        Log::Info("Hook installed");
    }
}