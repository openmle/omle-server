#ifndef OMLE_SERVER_REST_HANDLERS_H_
#define OMLE_SERVER_REST_HANDLERS_H_

#include <memory>

#include "omle_server/batcher.h"
#include "omle_server/model_registry.h"
#include "omle_server/server_config.h"

namespace omle_server {

void register_rest_handlers(std::shared_ptr<ModelRegistry> registry,
                            std::shared_ptr<BatcherRegistry> batchers,
                            const ServerConfig& cfg);

}  // namespace omle_server

#endif  // OMLE_SERVER_REST_HANDLERS_H_
