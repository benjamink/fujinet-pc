#include "httpService.h"

#include <sstream>
#include <vector>
#include <map>

#include "../../include/debug.h"

#include "fnSystem.h"
#include "fnConfig.h"
#include "fnDummyWiFi.h"
#include "fnFsSPIFFS.h"
#include "modem.h"
#include "printer.h"
#include "fuji.h"

#include "httpServiceConfigurator.h"
#include "httpServiceParser.h"
#include "httpServiceBrowser.h"



using namespace std;

// Global HTTPD
fnHttpService fnHTTPD;

/* Send some meaningful(?) error message to client
*/
void fnHttpService::return_http_error(struct mg_connection *c, _fnwserr errnum)
{
    const char *message;

    switch (errnum)
    {
    case fnwserr_fileopen:
        message = MSG_ERR_OPENING_FILE;
        break;
    case fnwserr_memory:
        message = MSG_ERR_OUT_OF_MEMORY;
        break;
    default:
        message = MSG_ERR_UNEXPECTED_HTTPD;
        break;
    }
    // httpd_resp_send(req, message, strlen(message));
    mg_http_reply(c, 400, "", "%s\n", message);
}

const char *fnHttpService::find_mimetype_str(const char *extension)
{
    static std::map<std::string, std::string> mime_map{
        {"html", "text/html"},
        {"css", "text/css"},
        {"png", "image/png"},
        {"jpg", "image/jpeg"},
        {"gif", "image/gif"},
        {"svg", "image/svg+xml"},
        {"pdf", "application/pdf"},
        {"ico", "image/x-icon"},
        {"txt", "text/plain"},
        {"bin", "application/octet-stream"},
        {"js", "text/javascript"},
        {"com", "application/octet-stream"},
        {"bin", "application/octet-stream"},
        {"exe", "application/octet-stream"},
        {"xex", "application/octet-stream"},
        {"atr", "application/octet-stream"},
        {"atx", "application/octet-stream"},
        {"cas", "application/octet-stream"},
        {"tur", "application/octet-stream"},
        {"wav", "audio/wav"},
        {"atascii", "application/octet-stream"}};

    if (extension != NULL)
    {
        std::map<std::string, std::string>::iterator mmatch;

        mmatch = mime_map.find(extension);
        if (mmatch != mime_map.end())
            return mmatch->second.c_str();
    }
    return NULL;
}

const char *fnHttpService::get_extension(const char *filename)
{
    const char *result = strrchr(filename, '.');
    if (result != NULL)
        return ++result;
    return NULL;
}

const char *fnHttpService::get_basename(const char *filepath)
{
    const char *result = strrchr(filepath, '/');
    if (result != NULL)
        return ++result;
    return filepath;
}

/* Set the response content type based on the file being sent.
*  Just using the file extension
*  If nothing is set here, the default is 'text/html'
*/
void fnHttpService::set_file_content_type(struct mg_connection *c, const char *filepath)
{
    // Find the current file extension
    const char *dot = get_extension(filepath);
    if (dot != NULL)
    {
        const char *mimetype = find_mimetype_str(dot);
        if (mimetype)
            mg_printf(c, "Content-Type: %s\r\n", mimetype);
    }
}

/* Send content of given file out to client
*/
void fnHttpService::send_file_parsed(struct mg_connection *c, const char *filename)
{
    // Build the full file path
    string fpath = FNWS_FILE_ROOT;
    // Trim any '/' prefix before adding it to the base directory
    while (*filename == '/')
        filename++;
    fpath += filename;

    Debug_printf("Opening file for parsing: '%s'\n", fpath.c_str());

    _fnwserr err = fnwserr_noerrr;

    // Retrieve server state
    serverstate *pState = &fnHTTPD.state; // ops TODO
    FILE *fInput = pState->_FS->file_open(fpath.c_str());

    if (fInput == nullptr)
    {
        Debug_println("Failed to open file for parsing");
        err = fnwserr_fileopen;
    }
    else
    {
        // We're going to load the whole thing into memory, so watch out for big files!
        size_t sz = FileSystem::filesize(fInput) + 1;
        char *buf = (char *)calloc(sz, 1);
        if (buf == NULL)
        {
            Debug_printf("Couldn't allocate %u bytes to load file contents!\n", (unsigned)sz);
            err = fnwserr_memory;
        }
        else
        {
            fread(buf, 1, sz, fInput);
            string contents(buf);
            free(buf);
            contents = fnHttpServiceParser::parse_contents(contents);

            mg_printf(c, "HTTP/1.1 200 OK\r\n");
            // Set the response content type
            set_file_content_type(c, fpath.c_str());
            // Set the expected length of the content
            size_t len = contents.length();
            mg_printf(c, "Content-Length: %lu\r\n\r\n", (unsigned long)len);
            // Send parsed content
            mg_send(c, contents.c_str(), len);
        }
    }

    if (fInput != nullptr)
        fclose(fInput);

    if (err != fnwserr_noerrr)
        return_http_error(c, err);
}

/* Send file content after parsing for replaceable strings
*/
void fnHttpService::send_file(struct mg_connection *c, const char *filename)
{
    // Build the full file path
    string fpath = FNWS_FILE_ROOT;
    // Trim any '/' prefix before adding it to the base directory
    while (*filename == '/')
        filename++;
    fpath += filename;


    // Retrieve server state
    serverstate *pState = &fnHTTPD.state; // ops TODO

    FILE *fInput = pState->_FS->file_open(fpath.c_str());
    if (fInput == nullptr)
    {
        Debug_printf("Failed to open file for sending: '%s'\n", fpath.c_str());
        return_http_error(c, fnwserr_fileopen);
    }
    else
    {
        mg_printf(c, "HTTP/1.1 200 OK\r\n");
        // Set the response content type
        set_file_content_type(c, fpath.c_str());
        // Set the expected length of the content
        mg_printf(c, "Content-Length: %lu\r\n\r\n", (unsigned long)FileSystem::filesize(fInput));

        // Send the file content out in chunks
        char *buf = (char *)malloc(FNWS_SEND_BUFF_SIZE);
        size_t count = 0;
        do
        {
            count = fread((uint8_t *)buf, 1, FNWS_SEND_BUFF_SIZE, fInput);
            mg_send(c, buf, count);
        } while (count > 0);
        free(buf);
        fclose(fInput);
    }
}

int fnHttpService::redirect_or_result(mg_connection *c, mg_http_message *hm, int result)
{
    // get "redirect" query variable
    char redirect[10] = "";
    mg_http_get_var(&hm->query, "redirect", redirect, sizeof(redirect));
    if (atoi(redirect))
    {
        // Redirect back to the main page
        mg_printf(c, "HTTP/1.1 303 See Other\r\nLocation: /\r\nContent-Length: 0\r\n\r\n");
    }
    else
    {
        mg_http_reply(c, 200, "", "{\"result\": %d}\n", result); // send reply
    }
    return result;
}

// void fnHttpService::parse_query(httpd_req_t *req, queryparts *results)
// {
//     results->full_uri += req->uri;
//     // See if we have any arguments
//     int path_end = results->full_uri.find_first_of('?');
//     if (path_end < 0)
//     {
//         results->path += results->full_uri;
//         return;
//     }
//     results->path += results->full_uri.substr(0, path_end - 1);
//     results->query += results->full_uri.substr(path_end + 1);
//     // TO DO: parse arguments, but we've no need for them yet
// }

// esp_err_t fnHttpService::get_handler_index(httpd_req_t *req)
// {
//     Debug_printf("Index request handler %p\n", xTaskGetCurrentTaskHandle());

//     send_file(req, "index.html");
//     return ESP_OK;
// }

// esp_err_t fnHttpService::get_handler_test(httpd_req_t *req)
// {
//     TaskHandle_t task = xTaskGetCurrentTaskHandle();
//     Debug_printf("Test request handler %p\n", task);

//     //Debug_printf("WiFI handle %p\n", handle_WiFi);
//     //vTaskPrioritySet(handle_WiFi, 5);

//     // Send the file content out in chunks
//     char testln[100];
//     for (int i = 0; i < 2000; i++)
//     {
//         int z = sprintf(testln, "%04d %06lu %p 0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz<br/>\n",
//                         i, fnSystem.millis() / 100, task);
//         httpd_resp_send_chunk(req, testln, z);
//     }
//     httpd_resp_send_chunk(req, nullptr, 0);

//     //vTaskPrioritySet(handle_WiFi, 23);

//     Debug_println("Test completed");
//     return ESP_OK;
// }

// esp_err_t fnHttpService::get_handler_file_in_query(httpd_req_t *req)
// {
//     //Debug_printf("File_in_query request handler '%s'\n", req->uri);

//     // Get the file to send from the query
//     queryparts qp;
//     parse_query(req, &qp);
//     send_file(req, qp.query.c_str());

//     return ESP_OK;
// }

// esp_err_t fnHttpService::get_handler_file_in_path(httpd_req_t *req)
// {
//     //Debug_printf("File_in_path request handler '%s'\n", req->uri);

//     // Get the file to send from the query
//     queryparts qp;
//     parse_query(req, &qp);
//     send_file(req, qp.path.c_str());

//     return ESP_OK;
// }

int fnHttpService::get_handler_print(struct mg_connection *c)
{
    Debug_println("Print request handler");

    uint64_t now = fnSystem.millis();
    // Get a pointer to the current (only) printer
    sioPrinter *printer = (sioPrinter *)fnPrinters.get_ptr(0);

    if (now - printer->lastPrintTime() < PRINTER_BUSY_TIME)
    {
        _fnwserr err = fnwserr_post_fail;
        return_http_error(c, err);
        return -1; //ESP_FAIL;
    }
    // Get printer emulator pointer from sioP (which is now extern)
    printer_emu *currentPrinter = printer->getPrinterPtr();

    // Build a print output name
    const char *exts;

    bool sendAsAttachment = true;

    // Choose an extension based on current printer papertype
    switch (currentPrinter->getPaperType())
    {
    case RAW:
        exts = "bin";
        break;
    case TRIM:
        exts = "atascii";
        break;
    case ASCII:
        exts = "txt";
        sendAsAttachment = false;
        break;
    case PDF:
        exts = "pdf";
        break;
    case SVG:
        exts = "svg";
        sendAsAttachment = false;
        break;
    case PNG:
        exts = "png";
        sendAsAttachment = false;
        break;
    case HTML:
    case HTML_ATASCII:
        exts = "html";
        sendAsAttachment = false;
        break;
    default:
        exts = "bin";
    }

    string filename = "printout.";
    filename += exts;

    // Tell printer to finish its output and get a read handle to the file
    FILE *poutput = currentPrinter->closeOutputAndProvideReadHandle();
    if (poutput == nullptr)
    {
        Debug_printf("Unable to open printer output\n");
        return_http_error(c, fnwserr_fileopen);
        return -1; //ESP_FAIL;
    }

    // Set the expected content type based on the filename/extension
    mg_printf(c, "HTTP/1.1 200 OK\r\n");
    set_file_content_type(c, filename.c_str());

    // char hdrval1[60];
    if (sendAsAttachment)
    {
        // Add a couple of attchment-specific details
        mg_printf(c, "Content-Disposition: attachment; filename=\"%s\"\r\n", filename.c_str());
    }
    mg_printf(c, "Content-Length: %lu\r\n\r\n", (unsigned long)FileSystem::filesize(poutput));

    // Finally, write the data
    // Send the file content out in chunks
    char *buf = (char *)malloc(FNWS_SEND_BUFF_SIZE);
    size_t count = 0, total = 0;
    do
    {
        count = fread((uint8_t *)buf, 1, FNWS_SEND_BUFF_SIZE, poutput);
        // count = currentPrinter->readFromOutput((uint8_t *)buf, FNWS_SEND_BUFF_SIZE);
        total += count;

        // Debug_printf("Read %u bytes from print file\n", count);

        mg_send(c, buf, count);
    } while (count > 0);

    Debug_printf("Sent %u bytes total from print file\n", (unsigned)total);

    free(buf);
    fclose(poutput);

    // Tell the printer it can start writing from the beginning
    printer->reset_printer(); // destroy,create new printer emulator object of previous type.

    Debug_println("Print request completed");

    return 0; //ESP_OK;
}

// esp_err_t fnHttpService::get_handler_modem_sniffer(httpd_req_t *req)
// {
//     Debug_printf("Modem Sniffer output request handler\n");
//     ModemSniffer *modemSniffer = sioR->get_modem_sniffer();
//     Debug_printf("Got modem Sniffer.\n");
//     time_t now = fnSystem.millis();

//     if (now - sioR->get_last_activity_time() < PRINTER_BUSY_TIME) // re-using printer timeout constant.
//     {
//         return_http_error(req, fnwserr_post_fail);
//         return ESP_FAIL;
//     }

//     set_file_content_type(req,"modem-sniffer.txt");

//     FILE *sOutput = modemSniffer->closeOutputAndProvideReadHandle();
//     Debug_printf("Got file handle %p\n",sOutput);
//     if(sOutput == nullptr)
//     {
//         return_http_error(req, fnwserr_post_fail);
//         return ESP_FAIL;
//     }
    
//     // Finally, write the data
//     // Send the file content out in chunks
//     char *buf = (char *)malloc(FNWS_SEND_BUFF_SIZE);
//     size_t count = 0, total = 0;
//     do
//     {
//         count = fread((uint8_t *)buf, 1, FNWS_SEND_BUFF_SIZE, sOutput);
//         // Debug_printf("fread %d, %d\n", count, errno);
//         total += count;

//         httpd_resp_send_chunk(req, buf, count);
//     } while (count > 0);

//     Debug_printf("Sent %u bytes total from sniffer file\n", total);

//     free(buf);
//     fclose(sOutput);

//     Debug_printf("Sniffer dump completed.\n");

//     return ESP_OK;
// }

// esp_err_t fnHttpService::post_handler_config(httpd_req_t *req)
int fnHttpService::post_handler_config(struct mg_connection *c, struct mg_http_message *hm)
{

    Debug_println("Post_config request handler");

    _fnwserr err = fnwserr_noerrr;

    if (fnHttpServiceConfigurator::process_config_post(hm->body.ptr, hm->body.len) < 0)
    {
        return_http_error(c, fnwserr_post_fail);
        return -1; //ESP_FAIL;
    }

    // Redirect back to the main page
    mg_printf(c, "HTTP/1.1 303 See Other\r\nLocation: /\r\nContent-Length: 0\r\n\r\n");

    return 0; //ESP_OK;
}


int fnHttpService::get_handler_browse(mg_connection *c, mg_http_message *hm)
{
    const char prefix[] = "/browse/host/";
    int prefixlen = sizeof(prefix) - 1;
    int pathlen = hm->uri.len - prefixlen -1;

    Debug_println("Browse request handler");
    if (pathlen >= 0 && strncmp(hm->uri.ptr, prefix, hm->uri.len))
    {
        const char *s = hm->uri.ptr + prefixlen;
        // /browse/host/{1..8}[/path/on/host...]
        if (*s >= '1' && *s <= '8' && (pathlen == 0 || s[1] == '/'))
        {
            int host_slot = *s - '1';
            fnHttpServiceBrowser::process_browse_get(c, hm, host_slot, s+1, pathlen);
        }
        else
        {
            mg_http_reply(c, 403, "", "Bad host slot\n");
        }
    }
    else
    {
        mg_http_reply(c, 403, "", "Bad browse request\n");
    }
    
    return 0;
}

int fnHttpService::get_handler_swap(mg_connection *c, mg_http_message *hm)
{
    // rotate disk images
    Debug_printf("Disk swap from webui\n");
    theFuji.image_rotate();
    return redirect_or_result(c, hm, 0);
}

int fnHttpService::get_handler_mount(mg_connection *c, mg_http_message *hm)
{
    char mountall[10] = "";
    mg_http_get_var(&hm->query, "mountall", mountall, sizeof(mountall));
    if (atoi(mountall))
    {
        // Mount all the things
        Debug_printf("Mount all from webui\n");
        theFuji.mount_all(false);
    }
    return redirect_or_result(c, hm, 0);
}

void fnHttpService::cb(struct mg_connection *c, int ev, void *ev_data, void *fn_data)
{
    static const char *s_root_dir = "data/www";

    if (ev == MG_EV_HTTP_MSG)
    {
        struct mg_http_message *hm = (struct mg_http_message *) ev_data;
        if (mg_http_match_uri(hm, "/test"))
        {
            // test handler
            mg_http_reply(c, 200, "", "{\"result\": %d}\n", 1);  // Serve REST
        }
        else if (mg_http_match_uri(hm, "/"))
        {
            // index handler
            send_file_parsed(c, "index.html");
        }
        else if (mg_http_match_uri(hm, "/file"))
        {
            // file handler
            char fname[60];
            if (hm->query.ptr != NULL && hm->query.len > 0 && hm->query.len < sizeof(fname))
            {
                strncpy(fname, hm->query.ptr, hm->query.len);
                fname[hm->query.len] = '\0';
                send_file(c, fname);
            }
            else
            {
                mg_http_reply(c, 400, "", "Bad file request\n");
            }
        }
        else if (mg_http_match_uri(hm, "/config"))
        {
            // config POST handler
            if (mg_vcasecmp(&hm->method, "POST") == 0)
            {
                post_handler_config(c, hm);
            }
            else
            {
                mg_http_reply(c, 400, "", "Bad config request\n");
            }
        }
        else if (mg_http_match_uri(hm, "/print"))
        {
            // print handler
            get_handler_print(c);
        }
        else if (mg_http_match_uri(hm, "/browse/#"))
        {
            // browse handler
            get_handler_browse(c, hm);
        }
        else if (mg_http_match_uri(hm, "/swap"))
        {
            // browse handler
            get_handler_swap(c, hm);
        }
        else if (mg_http_match_uri(hm, "/mount"))
        {
            // browse handler
            get_handler_mount(c, hm);
        }
        else if (mg_http_match_uri(hm, "/restart"))
        {
            // get "exit" query variable
            char exit[10] = "";
            mg_http_get_var(&hm->query, "exit", exit, sizeof(exit));
            if (atoi(exit)) 
            {
                mg_http_reply(c, 200, "", "{\"result\": %d}\n", 1); // send reply
                fnSystem.reboot(500, false); // deferred exit with code 0
            }
            else
            {
                // load restart page into browser
                send_file(c, "restart.html");
                // keep running for a while to transfer restart.html page
                fnSystem.reboot(500, true); // deferred exit with code 75 -> should be started again
            }
        }
        else
        // default handler, serve static content of www firectory
        {
            struct mg_http_serve_opts opts = {s_root_dir, NULL};
            mg_http_serve_dir(c, (mg_http_message*)ev_data, &opts);
        }
    }
    (void) fn_data;
}

struct mg_mgr * fnHttpService::start_server(serverstate &srvstate)
{
    std::string s_listening_address = Config.get_general_interface_url();

    static struct mg_mgr s_mgr;

    struct mg_connection *c;

    if (!fnWiFi.connected())
    {
        Debug_println("WiFi not connected - aborting web server startup");
        return nullptr;
    }

    // Set filesystem where we expect to find our static files
    srvstate._FS = &fnSPIFFS;

    Debug_printf("Starting web server %s\n", s_listening_address.c_str());

    mg_mgr_init(&s_mgr);

    if ((c = mg_http_listen(&s_mgr, s_listening_address.c_str(), cb, &s_mgr)) != nullptr)
    {
        srvstate.hServer = &s_mgr;
    }
    else
    {
        srvstate.hServer = nullptr;
        Debug_println("Error starting web server!");
    }
    return srvstate.hServer;
}


/* Set up and start the web server
 */
void fnHttpService::start()
{
    if (state.hServer != nullptr)
    {
        Debug_println("httpServiceInit: We already have a web server handle - aborting");
        return;
    }

    // Register event notifications to let us know when WiFi is up/down
    // Missing the constants used here.  Need to find that...
    // esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, &(state.hServer));
    // esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &(state.hServer));

    // Go ahead and attempt starting the server for the first time
    start_server(state);
}

void fnHttpService::stop()
{
    if (state.hServer != nullptr)
    {
        Debug_println("Stopping web service");
        // httpd_stop(state.hServer);
        mg_mgr_free(state.hServer);
        state._FS = nullptr;
        state.hServer = nullptr;
    }
}

void fnHttpService::service()
{
    if (state.hServer != nullptr)
        mg_mgr_poll(state.hServer, 0);
}
