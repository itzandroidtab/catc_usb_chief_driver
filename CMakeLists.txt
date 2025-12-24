cmake_minimum_required(VERSION 3.7)

project(usbchief)

# Set the SDK_ROOT variable if not already set
if (NOT SDK_ROOT)
    # Set the Windows DDK/WDK root path
    set(SDK_ROOT "D:/development/software/WinDDK/7600.16385.1")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)

# Remove default runtime check flags that are incompatible with DDK
string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")

set(SOURCES
    chief/driver.cpp
    chief/major_functions.cpp
    chief/spinlock.cpp
    chief/usb.cpp
)

# add the sources to create the executable
add_executable(usbchief ${SOURCES})

# Set output name to .sys for driver
set_target_properties(usbchief PROPERTIES 
    OUTPUT_NAME "usbchief"
    SUFFIX ".sys"
)

# include the source folder
target_include_directories(usbchief PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(usbchief PUBLIC "${SDK_ROOT}/inc/ddk")
target_include_directories(usbchief PUBLIC "${SDK_ROOT}/inc/crt")
target_include_directories(usbchief PUBLIC "${SDK_ROOT}/inc/api")

# Define target architecture for Windows DDK
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    # 64-bit build
    target_compile_definitions(usbchief PRIVATE _AMD64_ _WIN64 _WIN64_ STD_CALL _X86AMD64_)
    target_link_directories(usbchief PRIVATE "${SDK_ROOT}/lib/win7/amd64")
    target_link_directories(usbchief PRIVATE "${SDK_ROOT}/lib/crt/amd64")
else()
    # 32-bit build
    target_compile_definitions(usbchief PRIVATE _X86_ _WIN32 STD_CALL)
    target_link_directories(usbchief PRIVATE "${SDK_ROOT}/lib/win7/i386")
    target_link_directories(usbchief PRIVATE "${SDK_ROOT}/lib/crt/i386")
endif()

# Driver-specific compile definitions
target_compile_definitions(usbchief PRIVATE 
    _WIN32_WINNT=0x0601
    WINVER=0x0601
    NTDDI_VERSION=0x06010000
    DBG=1
    _KERNEL_MODE
)

# Disable runtime checks for DDK compatibility
target_compile_options(usbchief PRIVATE 
    /GS-        # Disable security checks
    /Gz         # Use __stdcall calling convention
    /Zp8        # 8-byte struct alignment
    /Zc:wchar_t-  # Treat wchar_t as built-in type
    /Zi         # Generate complete debug information (PDB)
)

# Link kernel libraries
target_link_libraries(usbchief ntoskrnl.lib hal.lib usbd.lib)

# Driver-specific linker options
target_link_options(usbchief PRIVATE
    /DRIVER
    /SUBSYSTEM:NATIVE
    /ENTRY:DriverEntry
    /NODEFAULTLIB
    /MERGE:.rdata=.text
    /MANIFEST:NO
)