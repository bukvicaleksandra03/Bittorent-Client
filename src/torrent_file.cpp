#include "torrent_file.h"

#include <algorithm>

#include "logger.h"
#include "utils.h"

TorrentFile::TorrentFile(std::shared_ptr<BDict> metadata_dict,
                         const std::vector<uint8_t>& info_bytes)
{
    // Calculate info_hash from raw info dictionary bytes
    if (info_bytes.empty())
    {
        LOG_E("No info dictionary bytes provided for hash calculation");
        LOG_AND_THROW(
            "Cannot calculate info_hash: no info bytes provided");
    }
    info_hash = crypto::sha1(info_bytes);
    LOG_I("Info hash: " + crypto::to_hex(info_hash));

    // Announce
    // TODO - BEP 19 - no need for announce field
    if (!metadata_dict->has_key("announce"))
    {
        LOG_E("Missing mandatory 'announce' field");
        LOG_AND_THROW(
            "The torrent file provided doesn't contain the mandatory announce "
            "field.");
    }
    std::string announce = metadata_dict->get_val<BString>("announce")->content;
    announce_tracker = extract_tracker_information(announce);

    // Announce list
    if (metadata_dict->has_key("announce-list"))
    {
        auto list_of_lists = metadata_dict->get_val<BList>("announce-list");
        for (auto& list_ptr : list_of_lists->content)
        {
            std::vector<std::string> vec;
            vec = load_list_of_bstrings(as<BList>(list_ptr.get()));

            std::vector<TrackerDetails> td_list;
            for (const auto& v : vec)
            {
                TrackerDetails td = extract_tracker_information(v);
                td_list.push_back(td);
            }
            announce_list_trackers.push_back(td_list);
        }
    }

    // Load the info dictionary fields
    if (!metadata_dict->has_key("info"))
    {
        LOG_E("Missing mandatory 'info' dictionary");
        LOG_AND_THROW(
            "The torrent file provided doesn't contain the mandatory \"info\" "
            "directory.");
    }
    BDict* info_dict = metadata_dict->get_val<BDict>("info");

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
        LOG_AND_THROW(
            "Piece length of the torrent must be a power of 2.");
    }

    if (piece_size < 16 * 1024)
    {
        LOG_E("Piece length too small: " + std::to_string(piece_size) +
              " bytes");
        LOG_AND_THROW(
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
                LOG_AND_THROW(
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
    os << "Torrent name: " << torrent_name << std::endl;
    os << "Total size: " << total_size << std::endl;
    os << "Piece size: " << piece_size << std::endl;
    os << "Piece count: " << pieces.size() << std::endl;
    os << "Info hash: " << crypto::to_hex(info_hash) << std::endl;
    os << "Is private: " << (is_private ? "yes" : "no") << std::endl;
    os << "Is multifile: " << (is_multifile ? "yes" : "no") << std::endl;
    os << "Tracker (" << announce_tracker.to_string() << "):" << std::endl;
    os << "     Hostname: " << announce_tracker.hostname << std::endl;
    os << "     Port: " << announce_tracker.port << std::endl;
    os << "     Protocol: " << announce_tracker.protocol.scheme << std::endl;
    os << "     Path: " << announce_tracker.path << std::endl;
    if (!announce_list_trackers.empty())
    {
        os << "Announce list:" << std::endl;
        for (size_t tier = 0; tier < announce_list_trackers.size(); ++tier)
        {
            os << "  Tier " << tier + 1 << ": [";
            const auto& trackers = announce_list_trackers[tier];
            for (size_t i = 0; i < trackers.size(); ++i)
            {
                os << "\"" << trackers[i].to_string() << "\"";
                if (i + 1 < trackers.size())
                    os << ", ";
            }
            os << "]" << std::endl;
        }
    }
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

TrackerDetails TorrentFile::extract_tracker_information(
    const std::string& announce)
{
    TrackerDetails td;

    size_t start = announce.find("://");
    if (start == std::string::npos)
        LOG_AND_THROW("Invalid announce URL: " + announce);

    td.protocol = TrackerProtocol::from_scheme(announce.substr(0, start));

    start += 3;  // Skip "://"
    size_t end = announce.find('/', start);
    if (end == std::string::npos)
        end = announce.length();

    td.hostname = announce.substr(start, end - start);
    td.path = announce.substr(end);
    if (td.path.empty())
        td.path = "/";

    size_t colon = td.hostname.find(':');
    if (colon != std::string::npos)
    {
        td.port = std::stoi(td.hostname.substr(colon + 1));
        td.hostname = td.hostname.substr(0, colon);
    }
    else
    {
        td.port = td.protocol.default_port;
    }

    return td;
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