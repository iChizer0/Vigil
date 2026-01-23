#include <chrono>
#include <iomanip>
#include <iostream>
#include <thread>

#include "vigil/vigil.hpp"

void print_size(const auto &result)
{
    if (result.valid)
    {
        double size_mb = static_cast<double>(result.size) / (1024.0 * 1024.0);
        std::cout << "Index size: " << std::fixed << std::setprecision(2) << size_mb << " MB (" << result.size
                  << " bytes)" << std::endl;
    }
    else
    {
        std::cout << "Index invalid" << std::endl;
    }
}

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        std::cerr << "Usage: " << argv[0] << " <watch_path>" << std::endl;
        std::cerr << "  watch_path: Directory to index and watch" << std::endl;
        std::cerr << "  (mount point is auto-detected)" << std::endl;
        return 1;
    }

    const char *watch_path = argv[1];

    vg_config_t cfg = {};
    vigil::Context ctx(cfg);

    try
    {
        vigil::index_manager::IndexManager manager(ctx, watch_path);
        {
            std::cout << "Starting index manager:" << std::endl;
            std::cout << "  Watch path:  " << watch_path << std::endl;
            std::cout << "  Mount point: " << manager.mountPoint().string() << " (auto-detected)" << std::endl;

            // Wait for initial indexing to complete
            std::cout << "Waiting for initial index..." << std::endl;
        }
        while (manager.indexing())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        print_size(manager.size());

        // Monitor for a while

        std::cout << "\nMonitoring for changes (press Ctrl+C to exit)..." << std::endl;

        while (true)
        {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            print_size(manager.size());

            if (manager.indexing())
            {
                std::cout << "  (currently re-indexing...)" << std::endl;
            }
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
