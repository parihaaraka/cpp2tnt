#include "connector.h"

#include "proto.h"

namespace tnt
{

    Connector::Connector()
        : iproto_writer(*(connection*)this)
    {
        on_response(bind(&Connector::OnResponse, this, placeholders::_1));
        on_closed(bind(&Connector::OnClosed, this));
        on_opened(bind(&Connector::OnOpened, this));
    }

    void Connector::OnResponse(wtf_buffer &buf)
    {
        isProcessingReply_ = true;
        mp_reader bunch(buf);
        while (mp_reader r = bunch.iproto_message())
        {
            try
            {
				auto encoded_header = r.read<mp_map_reader>();
                Header header;
                encoded_header[tnt::header_field::SYNC] >> header.sync;
                encoded_header[tnt::header_field::CODE] >> header.errCode;
                header.errCode &= 0x7fff;
                auto handler = handlers_.find(header.sync);
                if (handler == handlers_.end())
                    handle_error("unexpected response");
                else
                {
					auto encoded_body = r.read<mp_map_reader>();
                    handler->second.handler_(header, encoded_body, (void*)&handler->second.userData_);
                    //handlers_.erase(header.sync);
                    handlers_.erase(handler);
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
        isProcessingReply_ = false;
        if (isNeedsClose_)
        {
            connection::close(true, isNeedsReconnect_);
            isNeedsClose_ = false;
        }
    }

    void Connector::AddOnOpened(SimpleEventCallbak cb_)
    {
        onOpenedHandlers_.push_back(cb_);
    }

    void Connector::AddOnClosed(SimpleEventCallbak cb_)
    {
        onClosedHandlers_.push_back(cb_);
    }

    void Connector::OnOpened()
    {
        isConnected_ = true;
        for (auto& it : onOpenedHandlers_)
            it();
    }

    void Connector::OnClosed()
    {
        isConnected_ = false;
        wtf_buffer buff;
        mp_writer writer(buff);
        writer.begin_map(1);
        writer<<int(tnt::response_field::ERROR)<<"disconected";
        writer.finalize();
        Header header;
        header.errCode = 77;
        mp_reader reader(buff);
		auto body = reader.read<mp_map_reader>();
        for (auto& it : handlers_)
        {
            mp_map_reader body2 = body;
            header.sync = it.first;
            it.second.handler_(header, body2, &it.second.userData_);
        }
        handlers_.clear();
        for (auto& it : onClosedHandlers_)
            it();
    }

    void Connector::close(bool reconnect_soon) noexcept
    {
        if (isProcessingReply_)
        {
            isNeedsClose_ = true;
            isNeedsReconnect_ = reconnect_soon;
        }
        else
            connection::close(true, reconnect_soon);
    }

    bool Connector::IsConnected()
    {
        return isConnected_;
    }

}
