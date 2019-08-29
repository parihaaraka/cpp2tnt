#include "connector.h"

#include "proto.h"

namespace tnt
{

    Connector::Connector()
        : iproto_writer(*(connection*)this)
    {
        on_response(bind(&Connector::OnResponse, this, placeholders::_1));
    }

    void Connector::OnResponse(wtf_buffer &buf)
    {
        mp_reader bunch(buf);
        while (mp_reader r = bunch.iproto_message())
        {
            try
            {
                auto encoded_header = r.map();
                Header header;
                encoded_header[tnt::header_field::SYNC] >> header.sync;
                encoded_header[tnt::header_field::CODE] >> header.errCode;
                auto handler = handlers_.find(header.sync);
                if (handler == handlers_.end())
                    handle_error("unexpected response");
                else
                {
                    auto encoded_body = r.map();
                    handler->second(header, encoded_body);
                }
            }
            catch(const mp_reader_error &e)
            {
                handle_error(e.what());
            }
            catch(const exception &e)
            {
                handle_error(e.what());
            }
        }
        input_processed();
    }

}
