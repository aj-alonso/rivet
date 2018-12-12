/**********************************************************************
Copyright 2014-2016 The RIVET Developers. See the COPYRIGHT file at
the top-level directory of this distribution.

This file is part of RIVET.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
**********************************************************************/

#include "computation.h"
#include "dcel/arrangement.h"
#include "docopt.h"
#include "interface/input_manager.h"
#include "interface/input_parameters.h"
#include <boost/multi_array.hpp> // for print_betti
#include <interface/file_writer.h>

#include <dcel/grades.h>

#include "dcel/arrangement_message.h"
#include "api.h"

static const char USAGE[] =
    R"(RIVET: Rank Invariant Visualization and Exploration Tool

     The RIVET console application computes an augmented arrangement for
     2D persistent homology, which can be visualized with the RIVET GUI app.
     It also can perform standalone computation of Betti numbers, as well as 
     queries of an augmented arrangement for the barcodes of 1-D slices of a 2-D 
     persistence module.

    Usage:
      rivet_console (-h | --help)
      rivet_console --version
      rivet_console <input_file> --identify
      rivet_console <input_file> --minpres [-H <dimension>] [-V <verbosity>] [-x <xbins>] [-y <ybins>] [--koszul] [--num_threads=<num_threads>]
      rivet_console <input_file> [output_file] --betti [-H <dimension>] [-V <verbosity>] [-x <xbins>] [-y <ybins>] [--koszul] [--num_threads=<num_threads>]
      rivet_console <module_invariants_file> --bounds [-V <verbosity>]
      rivet_console <module_invariants_file> --barcodes <line_file> [-V <verbosity>]
      rivet_console <input_file> <module_invariants_file> [-H <dimension>] [-V <verbosity>] [-x <xbins>] [-y <ybins>] [-f <format>] [--binary] [--koszul] [--num_threads=<num_threads>]


    Options:
      <input_file>                             A text file with suitably formatted point cloud, bifiltration, or
                                               finite metric space as described at http://rivet.online/doc/input-data/
      <module_invariants_file>                 A module invariants file, as generated by this program by processing an
                                               <input_file>
      -h --help                                Show this screen
      --num_threads=<num_threads>              Max number of threads to use for parallel computations. 0 lets OpenMP decide. [default: 0]                             
      --version                                Show the version
      --identify                               Parse the file and print filetype information
      --binary                                 Include binary data (used by RIVET viewer only)
      -H <dimension> --homology=<dimension>    Dimension of homology to compute [default: 0]
      -x <xbins> --xbins=<xbins>               Number of bins in the x direction [default: 0]
      -y <ybins> --ybins=<ybins>               Number of bins in the y direction [default: 0]
      -V <verbosity> --verbosity=<verbosity>   Verbosity level: 0 (no console output) to 10 (lots of output) [default: 0]
      -f <format>                              Output format for file [default: msgpack]
      --minpres                                Print the minimal presentation, then exit.
      -b --betti                               Print dimension and Betti number information.  Optionally, also save this info
                                               to a file in a binary format for later viewing in the visualizer.  Then exit.
      --bounds                                 Print lower and upper bounds for the module in <module_invariants_file> and exit
      -k --koszul                              Use koszul homology-based algorithm to compute Betti numbers, instead of
                                               an approach based on computing presentations.
      --barcodes <line_file>                   Print barcodes for the line queries in line_file, then exit.
                                               line_file consists of pairs "m o", each representing a query line.
                                               m is the slope of the query line, given in degrees (0 to 90); o is the
                                               signed distance from the query line to the origin, where the sign is 
                                               positive if the line is above/left of the origin and negative otherwise.

                                               Example line_file contents:

                                                    #A line that starts with a # character will be ignored, as will blank lines

                                                    23 -0.22
                                                    67 1.88
                                                    10 0.92
                                                    #100 0.92   <-- will error if uncommented, 100 > 90

                                               RIVET will output one line of barcode information for each line
                                               in line_file, beginning by repeating the query. For example:

                                                    23 0.22: 88.1838 inf x1, 88.1838 91.2549 x5, 88.1838 89.7194 x12
                                                    67 0.88: 23.3613 inf x1
                                                    10 0.92: 11.9947 inf x1, 11.9947 19.9461 x2, 11.9947 16.4909 x1, 11.9947 13.0357 x4

)";

std::unique_ptr<ComputationResult>
from_messages(const TemplatePointsMessage &templatePointsMessage, const ArrangementMessage &arrangementMessage);

unsigned int get_uint_or_die(std::map<std::string, docopt::value>& args, const std::string& key)
{
    try {
        return static_cast<unsigned int>(args[key].asLong());
    } catch (std::exception& e) {
        std::cerr << "Argument " << key << " must be an integer";
        throw std::runtime_error("Failed to parse integer");
        //    exit(1);
    }
}

void write_msgpack_file(const std::string &file_name,
                        InputParameters const& params,
                        TemplatePointsMessage const& message,
                        ArrangementMessage const& arrangement)
{
    std::ofstream file(file_name, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Could not open " + file_name + " for writing.");
    }
    file << "RIVET_msgpack" << std::endl;
    msgpack::pack(file, params);
    msgpack::pack(file, message);
    msgpack::pack(file, arrangement);
    file.flush();
}

void write_template_points_file(const std::string &file_name,
                        TemplatePointsMessage const& message)
{
    std::ofstream file(file_name, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Could not open " + file_name + " for writing.");
    }
    file << "RIVET_msgpack" << std::endl;
    msgpack::pack(file, message);
    file.flush();
}

void print_dims(TemplatePointsMessage const& message, std::ostream& ostream)
{
    assert(message.homology_dimensions.dimensionality == 2);
    auto shape = message.homology_dimensions.shape();
    auto data = message.homology_dimensions.data();
    ostream << "Dimensions > 0:" << std::endl;

    for (unsigned long col = 0; col < shape[0]; col++) {
        for (unsigned long row = 0; row < shape[1]; row++) {
            unsigned dim = data[col * shape[1] + row];
            if (dim > 0) {
                ostream << "(" << col << ", " << row << ", " << dim << ")" << std::endl;
            }
        }
        ostream << std::endl;
    }
}

void print_betti(TemplatePointsMessage const& message, std::ostream& ostream)
{
    ostream << "Betti numbers:" << std::endl;
    for (int xi = 0; xi < 3; xi++) {
        ostream << "xi_" << xi << ":" << std::endl;
        for (auto point : message.template_points) {
            auto value = 0;
            if (xi == 0)
                value = point.zero;
            else if (xi == 1)
                value = point.one;
            else if (xi == 2)
                value = point.two;
            if (value > 0) {
                ostream << "(" << point.x << ", " << point.y << ", " << value << ")" << std::endl;
            }
        }
    }
}

void process_bounds(const ComputationResult &computation_result) {
    auto bounds = compute_bounds(computation_result);
    std::cout << std::setprecision(12) << "low: " << bounds.x_low << ", " << bounds.y_low << std::endl;
    std::cout << std::setprecision(12) << "high: " << bounds.x_high << ", " << bounds.y_high << std::endl;
}

void process_barcode_queries(const std::string &query_file_name, const ComputationResult& computation_result)
{
    std::ifstream query_file(query_file_name);
    if (!query_file.is_open()) {
        std::clog << "Could not open " << query_file_name << " for reading";
        return;
    }
    std::string line;
    std::vector<std::pair<double, double>> queries;
    int line_number = 0;
    while (std::getline(query_file, line)) {
        line_number++;
        line.erase(0, line.find_first_not_of(" \t"));
        if (line.empty() || line[0] == '#') {
            std::clog << "Skipped line " << line_number << ", comment or empty" << std::endl;
            continue;
        }
        std::istringstream iss(line);
        double angle;
        double offset;

        if (iss >> angle >> offset) {
            if (angle < 0 || angle > 90) {
                std::clog << "Angle on line " << line_number << " must be between 0 and 90" << std::endl;
                return;
            }

            queries.emplace_back(angle, offset);
        } else {
            std::clog << "Parse error on line " << line_number << std::endl;
            return;
        }
    }

    auto vec = query_barcodes(computation_result, queries);
    for(size_t i = 0; i < queries.size(); i++) {
        auto query = queries[i];
        auto angle = query.first;
        auto offset = query.second;
        std::cout << angle << " " << offset << ": ";
        auto barcode = vec[i].get();
        for (auto it = barcode->begin(); it != barcode->end(); it++) {
            auto bar = *it;
            std::cout << bar.birth << " ";

            if (bar.death == rivet::numeric::INFTY) {
                std::cout << "inf";
            } else {
                std::cout << bar.death;
            }
            std::cout << " x" << bar.multiplicity;
            if (std::next(it) != barcode->end()) {
                std::cout << ", ";
            }
        }
        std::cout << std::endl;
    }
}

void input_error(std::string message) {

    std::cerr << "INPUT ERROR: " << message << " :END" << std::endl;
    std::cerr << "Exiting" << std::endl
              << std::flush;
}

std::vector<std::string> temp_files;

void clean_temp_files() {
    for (auto file_name : temp_files) {
        std::remove(file_name.c_str());
    }
}

int main(int argc, char* argv[])
{
    InputParameters params; //parameter values stored here

    std::map<std::string, docopt::value> args = docopt::docopt(USAGE, { argv + 1, argv + argc }, true,
        "RIVET Console 1.0.0");

    std::shared_ptr<ArrangementMessage> arrangement_message;
    std::shared_ptr<TemplatePointsMessage> points_message;

    if (args["<input_file>"].isString()) {
        params.fileName = args["<input_file>"].asString();
    } else if (args["<module_invariants_file>"].isString()) {
        params.fileName = args["<module_invariants_file>"].asString();
    } else {
        //This should never happen if docopt is doing its job and the docstring is written correctly
        throw std::runtime_error("Either <input_file> or <module_invariants_file> must be supplied");
    }
    docopt::value& out_file_name = args["<module_invariants_file>"];
    if (out_file_name.isString()) {
        params.outputFile = out_file_name.asString();
    }
    params.dim = get_uint_or_die(args, "--homology");
    params.x_bins = get_uint_or_die(args, "--xbins");
    params.y_bins = get_uint_or_die(args, "--ybins");
    params.verbosity = get_uint_or_die(args, "--verbosity");
    params.outputFormat = args["-f"].asString();
    bool minpres_only = args["--minpres"].isBool() && args["--minpres"].asBool();
    bool betti_only = args["--betti"].isBool() && args["--betti"].asBool();
    bool binary = args["--binary"].isBool() && args["--binary"].asBool();
    bool identify = args["--identify"].isBool() && args["--identify"].asBool();
    bool bounds = args["--bounds"].isBool() && args["--bounds"].asBool();
    bool barcodes = args["--barcodes"].isString();
    bool koszul = args["--koszul"].isBool() && args["--koszul"].asBool();

    std::string slices;
    if (barcodes) {
        slices = args["--barcodes"].asString();
    }
    if (identify) {
        params.verbosity = 0;
    }
    int verbosity = params.verbosity;

    if (params.verbosity >= 8) {
        debug() << "X bins: " << params.x_bins;
        debug() << "Y bins: " << params.y_bins;
        debug() << "Verbosity: " << params.verbosity;
    }

    // Setup the requested number of threads to use for computations via OpenMP
    // This is will just fix the upper limit. Dynamic scheduling may decide to
    // run less threads.
    int num_threads = get_uint_or_die(args, "--num_threads");
    if(num_threads != 0) 
        omp_set_num_threads(num_threads);

    std::atexit(clean_temp_files);
    InputManager inputManager(params);
    Progress progress;
    Computation computation(verbosity, progress);
    if (binary || verbosity > 0) {
        progress.advanceProgressStage.connect([] {
            std::clog << "STAGE" << std::endl;

        });
        progress.progress.connect([](int amount) {
            std::clog << "PROGRESS " << amount << std::endl;
        });
        progress.setProgressMaximum.connect([](int amount) {
            std::clog << "STEPS_IN_STAGE " << amount << std::endl;
        });
    }
    computation.arrangement_ready.connect([&arrangement_message, &params, &binary, &verbosity](std::shared_ptr<Arrangement> arrangement) {
        arrangement_message.reset(new ArrangementMessage(*arrangement));
        if (binary) {
            std::cout << "ARRANGEMENT: " << params.outputFile << std::endl;
        } else if (verbosity > 0) {
            std::clog << "Wrote arrangement to " << params.outputFile << std::endl;
        }
    });


    //This function gets called by the computation object after the minimal
    //presentation is computed.  If minpres_only==true, it prints the
    //presentation and then exits RIVET console
    computation.minpres_ready.connect(
                                      [&minpres_only](const Presentation& pres) {
                                          if (minpres_only) {
                                              std::cout << "MINIMAL PRESENTATION:" << std::endl;
                                              pres.print_sparse();
                                              //TODO: this seems a little abrupt...
                                              std::cout.flush();
                                              exit(0);
                                          }
                                      });

    computation.template_points_ready.connect(

        //the argument to computation.template_points_ready.connect is the
        //following lambda function
        //TODO: Probably would improve readibility to actually make this a private
        //member function
        [&points_message, &binary, &minpres_only, &betti_only, &verbosity, &params](TemplatePointsMessage message) {
        points_message.reset(new TemplatePointsMessage(message));



        if (binary) {
            auto temp_name = params.outputFile + ".rivet-tmp";
            temp_files.push_back(temp_name);
            write_template_points_file(temp_name, *points_message);
            std::cout << "XI: " << temp_name << std::endl;
        }

        if (verbosity >= 4 || betti_only || minpres_only) {
            FileWriter::write_grades(std::cout, message.x_exact, message.y_exact);
        }
        if (betti_only) {
            print_dims(message, std::cout);
            std::cout << std::endl;
            print_betti(message, std::cout);
            
            //if an output file has been specified, then save the Betti numbers in an arrangement file (with no barcode templates)
            if (!params.outputFile.empty()) {
                std::ofstream file(params.outputFile);
                if (file.is_open()) {
                    std::vector<exact> emptyvec;
                    std::shared_ptr<Arrangement> temp_arrangement = std::make_shared<Arrangement>(emptyvec, emptyvec, verbosity);
                    std::shared_ptr<ArrangementMessage> temp_am = std::make_shared<ArrangementMessage>(*temp_arrangement);
                    if (verbosity > 0) {
                        debug() << "Writing file:" << params.outputFile;
                    }
                    write_msgpack_file(params.outputFile, params, *points_message, *temp_am);
                } else {
                    std::stringstream ss;
                    ss << "Error: Unable to write file:" << params.outputFile;
                    throw std::runtime_error(ss.str());
                }
            }

            //TODO: this seems a little abrupt...
            std::cout.flush();
            exit(0);
        }
    });

    if (identify) {
        auto file_type = inputManager.identify();
        std::cout << "FILE TYPE: " << file_type.identifier << std::endl;
        std::cout << "FILE TYPE DESCRIPTION: " << file_type.description << std::endl;
        std::cout << "RAW DATA: " << file_type.is_data << std::endl;
        std::cout.flush();
        return 0;
    }

    FileContent content;

    std::shared_ptr<Arrangement> arrangement;

//    try {
        content = inputManager.start(progress);
//    } catch (const std::exception& e) {
//        input_error(e.what());
//        return 1;
//    }
    if (params.verbosity >= 4) {
        debug() << "Input processed.";
    }

    if (barcodes || bounds) {
        if (content.type != FileContentType::PRECOMPUTED) {
            input_error("This function requires a RIVET module invariants file as input.");
            return 1;
        }
        if (barcodes) {
            if (!slices.empty()) {
                process_barcode_queries(slices, *content.result);
            }
        } else {
            process_bounds(*content.result);
        }
    } else {
        if (content.type != FileContentType::DATA) {
            input_error("This function requires a data file, not a RIVET module invariants file.");
            return 1;
        }
        content.result = computation.compute(*content.input_data, koszul);
        if (params.verbosity >= 2) {
            debug() << "Computation complete; augmented arrangement ready.";
        }
        arrangement = content.result->arrangement;
        if (params.verbosity >= 4) {
            arrangement->print_stats();
        }

    }
    //if an output file has been specified, then save the arrangement
    if (!params.outputFile.empty()) {
        std::ofstream file(params.outputFile);
        if (file.is_open()) {
            if (arrangement == nullptr) {
                arrangement.reset(new Arrangement());
            }
            if (verbosity > 0) {
                debug() << "Writing file:" << params.outputFile;
            }
            if (params.outputFormat == "R0") {
                FileWriter fw(params, *content.input_data, *(arrangement),
                              content.result->template_points);
                fw.write_augmented_arrangement(file);
            } else if (params.outputFormat == "msgpack") {
                write_msgpack_file(params.outputFile, params, *points_message, *arrangement_message);
            } else {
                throw std::runtime_error("Unsupported output format: " + params.outputFormat);
            }
        } else {
            std::stringstream ss;
            ss << "Error: Unable to write file:" << params.outputFile;
            throw std::runtime_error(ss.str());
        }
    }
    if (params.verbosity > 2) {
        debug() << "CONSOLE RIVET: Goodbye!";
    }
    return 0;
}
