#ifndef SYNC_H
#define SYNC_H

#include "connection.h"
#include "mp_reader.h"

class scope
{
public:
    scope(fu2::unique_function<void()> dtor);
    scope(const scope&) = delete;
    scope& operator=(const scope&) = delete;
    ~scope();
private:
    fu2::unique_function<void()> dtor;
};

scope run_loop();
bool open_tnt_connection(const std::string &connection_string);

/// do not return while there are uninvoked handlers
bool sync_tnt_request(fu2::unique_function<void(tnt::connection&)>);

void set_handler(uint64_t request_id, fu2::unique_function<bool(const mp_map_reader &header, const mp_map_reader &body)> handler);

#endif // SYNC_H
