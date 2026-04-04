#include "torrent_file.h"

#include "logger.h"
#include "utils.h"

TorrentFile::TorrentFile(std::shared_ptr<BDict> metadata_dict,
                         const std::vector<uint8_t>& info_bytes)
{
    // Calculate info_hash from raw info dictionary bytes
    if (info_bytes.empty())
    {
        LOG_E("No info dictionary bytes provided for hash calculation");
        throw std::runtime_error(
            "Cannot calculate info_hash: no info bytes provided");
    }
    info_hash = crypto::sha1(info_bytes);
    LOG_I("Info hash: " + crypto::to_hex(info_hash));

    // Announce
    // TODO - BEP 19 - no need for announce field
    if (!metadata_dict->has_key("announce"))
    {
        LOG_E("Missing mandatory 'announce' field");
        throw std::runtime_error(
            "The torrent file provided doesn't contain the mandatory announce "
            "field.");
    }
    announce = metadata_dict->get_val<BString>("announce")->content;
    LOG_D("Tracker: " + announce);
    extract_tracker_information(announce);

    // Announce list
    if (metadata_dict->has_key("announce-list"))
    {
        auto list_of_lists = metadata_dict->get_val<BList>("announce-list");
        for (auto& list_ptr : list_of_lists->content)
        {
            std::vector<std::string> vec;
            vec = load_list_of_bstrings(as<BList>(list_ptr.get()));
            announce_list.push_back(vec);
        }
    }

    // Load the info dictionary fields
    if (!metadata_dict->has_key("info"))
    {
        LOG_E("Missing mandatory 'info' dictionary");
        throw std::runtime_error(
            "The torrent file provided doesn't contain the mandatory \"info\" "
            "directory.");
    }
    BDict* info_dict = metadata_dict->get_val<BDict>("info");
    LOG_D("Info dictionary loaded");

    // Torrent name
    torrent_name = info_dict->get_val<BString>("name")->content;
    if (torrent_name.empty())
    {
        LOG_WARNING(
            "The torrent file provided doesn't contain the mandatory \"name\" "
            "field.");
    }

    // Load and verify piece length
    piece_size = info_dict->get_val<BInteger>("piece length")->value;
    if (!utils::is_power_of_two(piece_size))
    {
        LOG_E("Invalid piece length: " + std::to_string(piece_size) +
              " (not a power of 2)");
        throw std::runtime_error(
            "Piece length of the torrent must be a power of 2.");
    }

    if (piece_size < 16 * 1024)
    {
        LOG_E("Piece length too small: " + std::to_string(piece_size) +
              " bytes");
        throw std::runtime_error(
            "Piece length must be greater or equal to 16kB.");
    }
    LOG_D("Piece size: " + std::to_string(piece_size / 1024) + " KB");

    if (info_dict->has_key("files"))
        is_multifile = true;
    else
        is_multifile = false;

    LOG_D(is_multifile ? "Multi-file torrent detected"
                       : "Single-file torrent detected");

    if (!is_multifile)
    {
        total_size = info_dict->get_val<BInteger>("length")->value;
        LOG_D("File size: " + std::to_string(total_size / (1024 * 1024)) +
              " MB");
    }

    if (is_multifile)
    {
        total_size = 0;
        auto file_list = info_dict->get_val<BList>("files")->content;
        LOG_D("Loading " + std::to_string(file_list.size()) + " files...");
        for (const auto& elem : file_list)
        {
            struct TorrentFile::File f;
            if (elem->type() != BType::Type::Dictionary)
                throw std::runtime_error(
                    "Files entry should be a list of dictionaries.");
            auto file_dict = as<BDict>(elem.get());

            if (file_dict->has_key("crc32"))
                f.crc32 = file_dict->get_val<BString>("crc32")->content;

            f.length = file_dict->get_val<BInteger>("length")->value;
            total_size += f.length;

            if (file_dict->has_key("md5"))
                f.md5 = file_dict->get_val<BString>("md5")->content;
            if (file_dict->has_key("mtime"))
                f.mtime = file_dict->get_val<BString>("mtime")->content;
            if (file_dict->has_key("sha1"))
                f.sha1 = file_dict->get_val<BString>("sha1")->content;

            std::vector<std::string> parts_of_path =
                load_list_of_bstrings(file_dict->get_val<BList>("path"));
            for (const auto& part : parts_of_path)
            {
                f.path /= part;
            }
            files.push_back(f);
        }
        LOG_D("Total size: " + std::to_string(total_size / (1024 * 1024)) +
              " MB across " + std::to_string(files.size()) + " files");
    }

    // Load pieces string
    const std::string& pieces_str =
        info_dict->get_val<BString>("pieces")->content;
    size_t num_pieces = pieces_str.size() / 20;

    size_t expected_pieces = static_cast<size_t>(
        std::ceil(static_cast<double>(total_size) / piece_size));
    if (num_pieces != expected_pieces)
    {
        LOG_W("Piece count mismatch: found " + std::to_string(num_pieces) +
              ", expected " + std::to_string(expected_pieces));
        LOG_D("  Total size: " + std::to_string(total_size) + " bytes");
        LOG_D("  Piece size: " + std::to_string(piece_size) + " bytes");
    }

    // Resize the pieces vector to hold all piece hashes
    pieces.resize(num_pieces);

    for (size_t i = 0; i < num_pieces; ++i)
    {
        std::copy_n(
            reinterpret_cast<const uint8_t*>(pieces_str.data()) + i * 20,
            20,
            pieces[i].begin());
    }

    LOG_I("Loaded torrent: \"" + torrent_name + "\" (" +
          std::to_string(num_pieces) + " pieces)");
}

void TorrentFile::print(std::ostream& os)
{
    os << "Announce: " << announce << std::endl;
    os << "Torrent name: " << torrent_name << std::endl;
    os << "Total size: " << total_size << std::endl;
    os << "Piece size: " << piece_size << std::endl;
    os << "Piece count: " << pieces.size() << std::endl;
    os << "Info hash: " << crypto::to_hex(info_hash) << std::endl;
    os << "Is private: " << (is_private ? "yes" : "no") << std::endl;
    os << "Is multifile: " << (is_multifile ? "yes" : "no") << std::endl;
    os << "Tracker hostname: " << tracker_hostname << std::endl;
    os << "Tracker port: " << tracker_port << std::endl;
    os << "Tracker protocol: " << static_cast<int>(tracker_protocol)
       << std::endl;
    os << "Tracker path: " << tracker_path << std::endl;
    if (is_multifile)
    {
        os << "Number of files: " << files.size() << std::endl;
        os << "Files:" << std::endl;
        for (const auto& file : files)
        {
            os << "  " << file.path.string() << " (" << file.length << " bytes)"
               << std::endl;
        }
    }
}

void TorrentFile::extract_tracker_information(const std::string& announce)
{
    // Extract hostname from announce URL
    // e.g., "https://torrent.ubuntu.com/announce" -> "torrent.ubuntu.com"
    // Also extract port if it is present
    // e.g., "https://torrent.ubuntu.com:8080/announce" -> "8080"
    // e.g., "https://torrent.ubuntu.com/announce" -> "/announce"
    // if no path is present, use default path "/"
    size_t start = announce.find("://");
    if (start != std::string::npos)
    {
        std::string proto = announce.substr(0, start);
        if (proto == "udp")
        {
            tracker_protocol = TrackerProtocol::UDP;
        }
        else if (proto == "https")
        {
            tracker_protocol = TrackerProtocol::HTTPS;
        }
        else if (proto == "http")
        {
            tracker_protocol = TrackerProtocol::HTTP;
        }
        else
        {
            throw std::runtime_error("Unknown tracker protocol: " + proto);
        }
    }
    else
    {
        throw std::runtime_error("Invalid announce URL: " + announce);
    }
    start += 3;  // Skip "://"
    size_t end = announce.find('/', start);
    if (end == std::string::npos)
    {
        end = announce.length();
    }
    tracker_hostname = announce.substr(start, end - start);
    tracker_path = announce.substr(end);
    if (tracker_path.empty())
    {
        tracker_path = "/";
    }
    size_t colon = tracker_hostname.find(':');
    if (colon != std::string::npos)
    {
        tracker_port = std::stoi(tracker_hostname.substr(colon + 1));
        tracker_hostname = tracker_hostname.substr(0, colon);
    }
}

std::vector<std::string> TorrentFile::load_list_of_bstrings(BList* list)
{
    std::vector<std::string> ret;
    for (auto elem : list->content)
    {
        ret.push_back(as<BString>(elem.get())->content);
    }
    return ret;
}