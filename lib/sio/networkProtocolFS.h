#ifndef NETWORKPROTOCOLFS_H
#define NETWORKPROTOCOLFS_H

#include "networkProtocol.h"
#include "sio.h"
#include "EdUrlParser.h"

/**
 * Class for common logic for all N: accessible filesystems 
 */
class networkProtocolFS : public networkProtocol
{
public:
    virtual bool open(EdUrlParser *urlParser, cmdFrame_t *cmdFrame);
    virtual bool close();
    virtual bool read(uint8_t *rx_buf, unsigned short len);
    virtual bool write(uint8_t *tx_buf, unsigned short len);
    virtual bool status(uint8_t *status_buf);
    virtual bool special(uint8_t *sp_buf, unsigned short len, cmdFrame_t *cmdFrame);

    virtual bool del(EdUrlParser *urlParser, cmdFrame_t *cmdFrame) { return false; }
    virtual bool rename(EdUrlParser *urlParser, cmdFrame_t *cmdFrame) { return false; }
    virtual bool mkdir(EdUrlParser *urlParser, cmdFrame_t *cmdFrame) { return false; }
    virtual bool rmdir(EdUrlParser *urlParser, cmdFrame_t *cmdFrame) { return false; }
    virtual bool note(EdUrlParser *urlParser, cmdFrame_t *cmdFrame) { return false; }
    virtual bool point(EdUrlParser *urlParser, cmdFrame_t *cmdFrame) { return false; }
};

#endif /* NETWORKPROTOCOLFS_H */