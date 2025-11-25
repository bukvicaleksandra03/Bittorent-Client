#include "torrent_file.h"
#include "utils.h"

TorrentFile::TorrentFile(std::shared_ptr<BDict> metadata_dict)
{
    // Announce
    if (!metadata_dict->has_key("announce")) 
    {
        throw std::runtime_error("The torrent file provided doesn't contain the mandatory announce field.");
    }
    announce = metadata_dict->get_val<BString>("announce")->content;

    // Announce list
    if (metadata_dict->has_key("announce-list")) {
        auto list_of_lists = metadata_dict->get_val<BList>("announce-list");
        for (auto& list_ptr : list_of_lists->content) {
            std::vector<std::string> vec;
            vec = load_list_of_bstrings(as<BList>(list_ptr.get()));
            announce_list.push_back(vec);
        }
    }

    // Torrent name
    if (metadata_dict->has_key("title"))
        torrent_name = metadata_dict->get_val<BString>("title")->content;
    else if (metadata_dict->has_key("name"))
        torrent_name = metadata_dict->get_val<BString>("name")->content;
    else
        torrent_name = "";
    

    // Load the info dictionary fields
    if (!metadata_dict->has_key("info")) 
    {
        throw std::runtime_error("The torrent file provided doesn't contain the mandatory \"info\" directory.");
    }
    BDict* info_dict = metadata_dict->get_val<BDict>("info");

    // Load and verify piece length
    piece_size = info_dict->get_val<BInteger>("piece length")->value;
    if (!utils::is_power_of_two(piece_size)) {
        throw std::runtime_error("Piece length of the torrent must be a power of 2.");
    }
    if (piece_size < 16 * 1024) {
        throw std::runtime_error("Piece length must be greater or equal to 16kB.");
    }

    if (info_dict->has_key("files"))
        is_multifile = true;
    else
        is_multifile = false;

    if (!is_multifile)
    {
        total_size = info_dict->get_val<BInteger>("length")->value;
    }

    if (is_multifile)
    {
        total_size = 0;
        auto file_list = info_dict->get_val<BList>("files")->content;
        for (auto elem : file_list)
        {
            struct TorrentFile::File f;
            if (elem->type() != BType::Type::Dictionary)
                throw std::runtime_error("Files entry should be a list of dictionaries.");
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

            std::vector<std::string> parts_of_path = load_list_of_bstrings(file_dict->get_val<BList>("path"));
            for (const auto& part : parts_of_path) {
                f.path /= part; 
            }
        }
    }
   
    // Load pieces string
    const std::string& pieces_str = info_dict->get_val<BString>("pieces")->content;
    size_t num_pieces = pieces_str.size() / 20;

    if (num_pieces != std::ceil(static_cast<double>(total_size) / piece_size)) {
        std::cout << "Number of pieces is: " << num_pieces << std::endl;
        std::cout << "Total size: " << total_size << std::endl;
        std::cout << "Piece_size: " << piece_size << std::endl;
        std::cout << "std::ceil(total_size / piece_size) is: " << std::ceil(static_cast<double>(total_size) / piece_size) << std::endl;
    }

    std::vector<std::array<uint8_t, 20>> pieces(num_pieces);

    for (size_t i = 0; i < num_pieces; ++i) {
        std::copy_n(
            reinterpret_cast<const uint8_t*>(pieces_str.data()) + i*20,
            20,
            pieces[i].begin()
        );
    }
}

std::vector<std::string>
TorrentFile::load_list_of_bstrings(BList* list)
{
    std::vector<std::string> ret;
    for (auto elem : list->content)
    {
        ret.push_back(as<BString>(elem.get())->content);
    }
    return ret;
}