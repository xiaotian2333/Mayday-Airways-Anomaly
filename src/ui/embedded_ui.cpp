#include "render/dx12/embedded_host_internal.hpp"

#include "anomaly/host_ui_service.hpp"

namespace anomaly::embedded {

const AnomalyUiServiceV1* EmbeddedUiServiceTable() noexcept {
    return anomaly::HostUiServiceTable();
}

}  // namespace anomaly::embedded
