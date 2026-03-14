#include "tracker_request.h"

#include "logger.h"

TrackerRequest::TrackerRequest(const PeerId& peer_id,
                               const std::unique_ptr<TorrentFile>& tf)
    : _tracker_hostname(tf->get_tracker_hostname()),
      _tracker_path(tf->get_tracker_path()),
      _protocol(tf->get_tracker_protocol()),
      _info_hash(tf->get_info_hash()),
      _peer_id(peer_id),
      _peer_port(6881),
      _downloaded(0),
      _left(tf->get_total_size()),
      _uploaded(0)
{
    LOG_D("TrackerRequest created for " + _tracker_hostname + ":" +
          std::to_string(_tracker_port));
}
