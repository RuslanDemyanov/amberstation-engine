// Протокол между приложением и движком эмуляции (MIT, см. LICENSE).
// Подключают обе стороны: закрытое приложение и GPL-движок.
plugins {
    alias(libs.plugins.android.library)
}

android {
    namespace = "com.leomze.ps1.engine.api"
    compileSdk {
        version = release(37)
    }
    defaultConfig {
        minSdk = 26
    }
    buildFeatures {
        aidl = true
    }
}
