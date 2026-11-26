include(FetchContent)

function(wwmm_add_pair_crypto)
    # Keep upstream tools and tests out of the application build and install.
    FetchContent_Declare(boringssl
        URL https://codeload.github.com/google/boringssl/tar.gz/5e7f7792f2223216967aa9abc6be8e5b1727e41d
        URL_HASH SHA256=5991b09e2a23e5cc18eb7cc4373448f4124727000aaa3cc2c62c0e09aad9d0f1
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR wwmm-populate-only)
    FetchContent_MakeAvailable(boringssl)
    set(BUILD_TESTING OFF)
    set(BUILD_SHARED_LIBS OFF)
    set(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT FALSE)
    if(WIN32)
        set(OPENSSL_NO_ASM ON)
    endif()
    add_subdirectory("${boringssl_SOURCE_DIR}" "${boringssl_BINARY_DIR}" EXCLUDE_FROM_ALL)
    set(WWMM_CRYPTO_LICENSE "${boringssl_SOURCE_DIR}/LICENSE" PARENT_SCOPE)
endfunction()
