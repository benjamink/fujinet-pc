// #include <esp_system.h>
// #include <nvs_flash.h>
// #include <esp32/spiram.h>
// #ifdef ATARI
// #include <esp32/himem.h>
// #endif

#include <signal.h>
#include <unistd.h>

#include "debug.h"
#include "bus.h"
#include "device.h"
// #include "keys.h"
// #include "led.h"
#include "crypt.h"

#include "fnSystem.h"
#include "fnConfig.h"
#include "fnWiFi.h"

#include "fsFlash.h"
#include "fnFsSD.h"

#include "httpService.h"

#include "fnTaskManager.h"
#include "version.h"

#ifdef BLUETOOTH_SUPPORT
#include "fnBluetooth.h"
#endif

// fnSystem is declared and defined in fnSystem.h/cpp
// fnBtManager is declared and defined in fnBluetooth.h/cpp
// fnLedManager is declared and defined in led.h/cpp
// fnKeyManager is declared and defined in keys.h/cpp
// fnHTTPD is declared and defineid in HttpService.h/cpp

// sioFuji theFuji; // moved to fuji.h/.cpp

volatile sig_atomic_t fn_shutdown = 0;

void main_shutdown_handler()
{
    Debug_println("Shutdown handler called");
    // Give devices an opportunity to clean up before rebooting

    SYSTEM_BUS.shutdown();
}

void sighandler(int signum)
{
    fn_shutdown = 1 + fn_shutdown;
    if (fn_shutdown >= 3)
        _exit(EXIT_FAILURE); // emergency exit
}

void print_version()
{
    printf("FujiNet-PC " FN_VERSION_FULL "\n");
    printf("Version date: " FN_VERSION_DATE "\n");

    printf("Build: ");
#if defined(_WIN32)
    printf("Windows");
#elif defined(__linux__)
    printf("Linux");
#elif defined(__APPLE__)
    printf("macOS");
#else
    printf("unknown");
#endif
    printf("\n");

    printf("Platform: ");
#if defined(BUILD_ATARI)
    printf("Atari");
#elif defined(BUILD_ADAM)
    printf("Adam");
#elif defined(BUILD_APPLE)
    printf("Apple");
#elif defined(BUILD_MAC)
    printf("Mac");
#elif defined(BUILD_IEC)
    printf("Commodore");
#elif defined(BUILD_LYNX)
    printf("Lynx");
#elif defined(BUILD_S100)
    printf("S100");
#elif defined(BUILD_RS232)
    printf("RS232");
#elif defined(BUILD_CX16)
    printf("CX16");
#elif defined(BUILD_RC2014)
    printf("RC2014");
#elif defined(BUILD_H89)
    printf("H89");
#elif defined(BUILD_COCO)
    printf("TRS-80");
#else
    printf("unknown");
#endif
    printf("\n");
}

// Initial setup
void main_setup(int argc, char *argv[])
{
    // program arguments
    int opt;
    while ((opt = getopt(argc, argv, "Vu:c:s:")) != -1) {
        switch (opt) {
            case 'V':
                print_version();
                exit(EXIT_SUCCESS);
            case 'u':
                Config.store_general_interface_url(optarg);
                break;
            case 'c':
                Config.store_general_config_path(optarg);
                break;
            case 's':
                Config.store_general_SD_path(optarg);
                break;
            default: /* '?' */
                fprintf(stderr, "Usage: %s [-V] [-u URL] [-c config_file] [-s SD_directory]\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }

#ifdef DEBUG
    // fnUartDebug.begin(DEBUG_SPEED);
    unsigned long startms = fnSystem.millis();
    Debug_print("\n");
    Debug_print("\n");
    Debug_print("--~--~--~--\n");
    Debug_printf("FujiNet %s Started @ %lu\n", fnSystem.get_fujinet_version(), startms);
    Debug_printf("Starting heap: %u\n", fnSystem.get_free_heap_size());
#ifdef ATARI
    Debug_printf("PsramSize %u\n", fnSystem.get_psram_size());
    // Debug_printf("himem phys %u\n", esp_himem_get_phys_size());
    // Debug_printf("himem free %u\n", esp_himem_get_free_size());
    // Debug_printf("himem reserved %u\n", esp_himem_reserved_area_size());

    Debug_printf("himem phys 0\n");
    Debug_printf("himem free 0\n");
    Debug_printf("himem reserved 0\n");
#endif // ATARI
#endif // DEBUG

/*
    // Install a reboot handler
    esp_register_shutdown_handler(main_shutdown_handler);

    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        Debug_println("Erasing flash");
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);

    // Enable GPIO Interrupt Service Routine
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
*/

#if defined(_WIN32)
    // Initialize Winsock
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2,2), &wsaData);
    if (result != 0) 
    {
        Debug_printf("WSAStartup failed: %d\n", result);
        exit(EXIT_FAILURE);
    }
#endif

    atexit(main_shutdown_handler);
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);
#if defined(_WIN32)
    signal(SIGBREAK, sighandler);
#endif

    fnSystem.check_hardware_ver(); // Run early to determine correct FujiNet hardware
    Debug_printf("Detected Hardware Version: %s\r\n", fnSystem.get_hardware_ver_str());

    // fnKeyManager.setup();
    // fnLedManager.setup();

    fsFlash.start();
    fnSDFAT.start(Config.get_general_SD_path().c_str());

    // setup crypto key - must be done before loading the config
    crypto.setkey("FNK" + fnWiFi.get_mac_str());

    // Load our stored configuration
    Config.load();

    // Now that our main service is running, try connecting to WiFi or BlueTooth
    if (Config.get_bt_status())
    {
#ifdef BLUETOOTH_SUPPORT
        // Start SIO2BT mode if we were in it last shutdown
        fnLedManager.set(eLed::LED_BT, true); // BT LED ON
        fnBtManager.start();
#endif
    }
    else if (Config.get_wifi_enabled())
    {
        // Set up the WiFi adapter if enabled in config
        fnWiFi.start();
        // Go ahead and try reconnecting to WiFi
        fnWiFi.connect();
    }

#ifdef BUILD_ATARI
    theFuji.setup(&SIO);
    SIO.addDevice(&theFuji, SIO_DEVICEID_FUJINET); // the FUJINET!

    if (Config.get_apetime_enabled() == true)
        SIO.addDevice(&apeTime, SIO_DEVICEID_APETIME); // APETime

    // SIO.addDevice(&udpDev, SIO_DEVICEID_MIDI); // UDP/MIDI device

    pcLink.mount(1, Config.get_general_SD_path().c_str()); // mount SD as PCL1:
    SIO.addDevice(&pcLink, SIO_DEVICEID_PCLINK); // PCLink

    // Create a new printer object, setting its output depending on whether we have SD or not
    FileSystem *ptrfs = fnSDFAT.running() ? (FileSystem *)&fnSDFAT : (FileSystem *)&fsFlash;
    sioPrinter::printer_type ptype = Config.get_printer_type(0);
    if (ptype == sioPrinter::printer_type::PRINTER_INVALID)
        ptype = sioPrinter::printer_type::PRINTER_FILE_TRIM;

    Debug_printf("Creating a default printer using %s storage and type %d\r\n", ptrfs->typestring(), ptype);

    sioPrinter *ptr = new sioPrinter(ptrfs, ptype);
    fnPrinters.set_entry(0, ptr, ptype, Config.get_printer_port(0));

    SIO.addDevice(ptr, SIO_DEVICEID_PRINTER + fnPrinters.get_port(0)); // P:

    sioR = new modem(ptrfs, Config.get_modem_sniffer_enabled()); // Config/User selected sniffer enable
    sioR->set_uart(&fnSioCom);
    SIO.addDevice(sioR, SIO_DEVICEID_RS232); // R:

    // SIO.addDevice(&sioV, SIO_DEVICEID_FN_VOICE); // P3:

    SIO.addDevice(&sioZ, SIO_DEVICEID_CPM); // (ATR8000 CPM)

    // Go setup SIO
    SIO.setup();
#endif // BUILD_ATARI

#ifdef BUILD_COCO
    theFuji.setup(&DRIVEWIRE);
    DRIVEWIRE.setup();
#endif

#ifdef BUILD_IEC
    FileSystem *ptrfs = fnSDFAT.running() ? (FileSystem *)&fnSDFAT : (FileSystem *)&fsFlash;

    // Setup IEC Bus
    IEC.setup();
//    iecPrinter::printer_type ptype = Config.get_printer_type(0);
    iecPrinter::printer_type ptype = iecPrinter::printer_type::PRINTER_COMMODORE_MPS803; // temporary
    Debug_printf("Creating a default printer using %s storage and type %d\r\n", ptrfs->typestring(), ptype);
    iecPrinter *ptr = new iecPrinter(ptrfs, ptype);
    fnPrinters.set_entry(0, ptr, ptype, Config.get_printer_port(0));
    IEC.addDevice(ptr, 0x04); // add as device #4 for now
    theFuji.setup(&IEC);
    sioR = new iecModem(ptrfs, Config.get_modem_sniffer_enabled());

#endif // BUILD_IEC

#ifdef BUILD_LYNX
    theFuji.setup(&ComLynx);
    ComLynx.setup();
#endif

#ifdef BUILD_RS232
    theFuji.setup(&RS232);
    RS232.setup();
    RS232.addDevice(&theFuji,0x70);
#endif

#ifdef BUILD_RC2014
    theFuji.setup(&rc2014Bus);
    rc2014Bus.setup();

    FileSystem *ptrfs = fnSDFAT.running() ? (FileSystem *)&fnSDFAT : (FileSystem *)&fsFlash;
    rc2014Printer::printer_type ptype = Config.get_printer_type(0);
    if (ptype == rc2014Printer::printer_type::PRINTER_INVALID)
        ptype = rc2014Printer::printer_type::PRINTER_FILE_TRIM;

    Debug_printf("Creating a default printer using %s storage and type %d\r\n", ptrfs->typestring(), ptype);

    rc2014Printer *ptr = new rc2014Printer(ptrfs, ptype);
    fnPrinters.set_entry(0, ptr, ptype, Config.get_printer_port(0));

    rc2014Bus.addDevice(ptr, RC2014_DEVICEID_PRINTER + fnPrinters.get_port(0)); // P:

    sioR = new rc2014Modem(ptrfs, Config.get_modem_sniffer_enabled()); // Config/User selected sniffer enable
    rc2014Bus.addDevice(sioR, RC2014_DEVICEID_MODEM); // R:

#endif

#ifdef BUILD_H89
    theFuji.setup(&H89Bus);
    H89Bus.setup();

    FileSystem *ptrfs = fnSDFAT.running() ? (FileSystem *)&fnSDFAT : (FileSystem *)&fsFlash;
    H89Printer::printer_type ptype = Config.get_printer_type(0);
    if (ptype == H89Printer::printer_type::PRINTER_INVALID)
        ptype = H89Printer::printer_type::PRINTER_FILE_TRIM;

    Debug_printf("Creating a default printer using %s storage and type %d\r\n", ptrfs->typestring(), ptype);

    H89Printer *ptr = new H89Printer(ptrfs, ptype);
    fnPrinters.set_entry(0, ptr, ptype, Config.get_printer_port(0));

    // H89Bus.addDevice(ptr, H89_DEVICEID_PRINTER + fnPrinters.get_port(0)); // P:

    // H89R = new H89Modem(ptrfs, Config.get_modem_sniffer_enabled()); // Config/User selected sniffer enable
    // H89Bus.addDevice(H89R, H89_DEVICEID_MODEM); // R:

#endif

#ifdef BUILD_ADAM
    theFuji.setup(&AdamNet);
    AdamNet.setup();
    fnSDFAT.create_path("/FujiNet");

    Debug_printf("Adding virtual printer\r\n");
    FileSystem *ptrfs = fnSDFAT.running() ? (FileSystem *)&fnSDFAT : (FileSystem *)&fsFlash;
    adamPrinter::printer_type printer = Config.get_printer_type(0);
    adamPrinter *ptr = new adamPrinter(ptrfs, printer);
    fnPrinters.set_entry(0, ptr, printer, 0);
    AdamNet.addDevice(ptr, ADAMNET_DEVICE_ID_PRINTER);

    if (Config.get_printer_enabled())
        AdamNet.enableDevice(ADAMNET_DEVICE_ID_PRINTER);
    else
        AdamNet.disableDevice(ADAMNET_DEVICE_ID_PRINTER);

#ifdef VIRTUAL_ADAM_DEVICES
    Debug_printf("Physical Device Scanning...\r\n");
    sioQ = new adamQueryDevice();

#ifndef NO_VIRTUAL_KEYBOARD
    exists = sioQ->adamDeviceExists(ADAMNET_DEVICE_ID_KEYBOARD);
    if (!exists)
    {
        Debug_printf("Adding virtual keyboard\r\n");
        sioK = new adamKeyboard();
        AdamNet.addDevice(sioK, ADAMNET_DEVICE_ID_KEYBOARD);
    }
    else
        Debug_printf("Physical keyboard found\r\n");
#endif // NO_VIRTUAL_KEYBOARD

#endif // VIRTUAL_ADAM_DEVICES

#endif // BUILD_ADAM

#ifdef BUILD_APPLE

    iwmModem *sioR;
    FileSystem *ptrfs = fnSDFAT.running() ? (FileSystem *)&fnSDFAT : (FileSystem *)&fsFlash;
    sioR = new iwmModem(ptrfs, Config.get_modem_sniffer_enabled());
    IWM.addDevice(sioR,iwm_fujinet_type_t::Modem);
    iwmPrinter::printer_type ptype = Config.get_printer_type(0);
    iwmPrinter *ptr = new iwmPrinter(ptrfs, ptype);
    fnPrinters.set_entry(0, ptr, ptype, Config.get_printer_port(0));
    IWM.addDevice(ptr, iwm_fujinet_type_t::Printer);

    theFuji.setup(&IWM);
    IWM.setup(); // save device unit SP address somewhere and restore it after reboot?

#endif /* BUILD_APPLE */

#ifdef BUILD_MAC
    FileSystem *ptrfs = fnSDFAT.running() ? (FileSystem *)&fnSDFAT : (FileSystem *)&fsFlash;

    sioR = new macModem(ptrfs, Config.get_modem_sniffer_enabled());
    MAC.setup();
    theFuji.setup(&MAC);

#endif // BUILD_MAC

#ifdef BUILD_CX16
    theFuji.setup(&CX16);
    CX16.addDevice(&theFuji, CX16_DEVICEID_FUJINET); // the FUJINET!

    // Create a new printer object, setting its output depending on whether we have SD or not
    FileSystem *ptrfs = fnSDFAT.running() ? (FileSystem *)&fnSDFAT : (FileSystem *)&fsFlash;
    cx16Printer::printer_type ptype = Config.get_printer_type(0);
    if (ptype == cx16Printer::printer_type::PRINTER_INVALID)
        ptype = cx16Printer::printer_type::PRINTER_FILE_TRIM;

    Debug_printf("Creating a default printer using %s storage and type %d\r\n", ptrfs->typestring(), ptype);

    cx16Printer *ptr = new cx16Printer(ptrfs, ptype);
    fnPrinters.set_entry(0, ptr, ptype, Config.get_printer_port(0));

    CX16.addDevice(ptr, CX16_DEVICEID_PRINTER + fnPrinters.get_port(0)); // P:

    // Go setup SIO
    CX16.setup();
#endif

    fn_shutdown = 0;

#ifdef DEBUG
    unsigned long endms = fnSystem.millis();
    Debug_printf("Available heap: %u\n", fnSystem.get_free_heap_size());
    Debug_printf("Setup complete @ %lu (%lums)\n", endms, endms - startms);
#endif
}

#ifdef BUILD_S100

// theFuji.setup(&s100Bus);
// SYSTEM_BUS.setup();

#endif /* BUILD_S100*/

// Main high-priority service loop
void fn_service_loop(void *param)
{
    while (!fn_shutdown)
    {
        // We don't have any delays in this loop, so IDLE threads will be starved
        // Shouldn't be a problem, but something to keep in mind...

        // Go service BT if it's active
#ifdef BLUETOOTH_SUPPORT
        if (fnBtManager.isActive())
            fnBtManager.service();
        else
    #endif
        SYSTEM_BUS.service();

        fnHTTPD.service();

        taskMgr.service();

        if (fnSystem.check_deferred_reboot())
        {
            // stop the web server first
            // web server is tested by script in restart.html to check if the program is running again
            fnHTTPD.stop();
            // exit the program with special exit code (75)
            // indicate to the controlling script (run-fujinet) that this program (fujinet) should be started again
            fnSystem.reboot(); // calls exit(75)
        }
    }
}

/*
 * This is the start/entry point for an ESP-IDF program (must use "C" linkage)
 */
/*
extern "C"
{
    void app_main()
    {
        // cppcheck-suppress "unusedFunction"
        // Call our setup routine
        //main_setup();

// Create a new high-priority task to handle the main loop
// This is assigned to CPU1; the WiFi task ends up on CPU0
#define MAIN_STACKSIZE 32768
#ifdef BUILD_ADAM
#define MAIN_PRIORITY 30
#else
#define MAIN_PRIORITY 17
#endif
#define MAIN_CPUAFFINITY 1

        xTaskCreatePinnedToCore(fn_service_loop, "fnLoop",
                                MAIN_STACKSIZE, nullptr, MAIN_PRIORITY, nullptr, MAIN_CPUAFFINITY);

        // Delete app_main() task since we no longer need it
        vTaskDelete(NULL);
    }
}
*/

int main(int argc, char *argv[])
{
    // Call our setup routine
    main_setup(argc, argv);
    // Enter service loop
    fn_service_loop(nullptr);
    return EXIT_SUCCESS;
}
