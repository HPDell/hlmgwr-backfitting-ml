#include <string>
#include <iostream>
#include <armadillo>
#include <boost/program_options.hpp>
#include "hlmgwr.h"

using namespace std;
using namespace arma;

int main(int argc, char *argv[])
{
/// Command Line Options
    HLMGWROptions options;
    boost::program_options::options_description desc("Gradient Descent Solution for HLMGWR");
    desc.add_options()
        ("data-dir,d", boost::program_options::value<string>(), "Data directory")
        ("bandwidth,b", boost::program_options::value<double>(), "Bandwidth")
        ("alpha,a", boost::program_options::value<double>()->default_value(0.01), "Learning speed")
        ("eps-iter,e", boost::program_options::value<double>()->default_value(1e-6, "1e-6"), "Coverage threshold")
        ("eps-gradient,g", boost::program_options::value<double>()->default_value(1e-6, "1e-6"), "Minimize Log-likelihood threshold")
        ("max-iters,m", boost::program_options::value<size_t>()->default_value(1e6, "1e6"), "Maximum iteration")
        ("max-retries,r", boost::program_options::value<size_t>()->default_value(10), "Maximum retry times when algorithm seems to diverge")
        ("ml-beta", "Whether use maximum likelihood to estimate beta")
        ("verbose,v", boost::program_options::value<size_t>()->default_value(0), "Print algorithm details")
        ("v1", "Only print details of the back-fitting part")
        ("v2", "Print both details of the back-fitting part and the maximum likelihood part")
        ("help,h", "Print help.");
    boost::program_options::variables_map var_map;
    boost::program_options::store(boost::program_options::parse_command_line(argc, argv, desc), var_map);
    boost::program_options::notify(var_map);
    if (var_map.count("help") > 0)
    {
        cout << desc << endl;
        return 0;
    }
    if (var_map.count("bandwidth") <= 0)
    {
        cout << "Bandwidth must be specified!" << endl;
        return 1;
    }
    string data_dir;
    if (var_map.count("data-dir") > 0) data_dir = var_map["data-dir"].as<string>();
    else 
    {
        cout << "Argument data-dir must be specified!" << endl;
        return 2;
    }
    if (var_map.count("alpha") > 0) options.alpha = var_map["alpha"].as<double>();
    if (var_map.count("eps-iter") > 0) options.eps_iter = var_map["eps-iter"].as<double>();
    if (var_map.count("eps-gradient") > 0) options.eps_gradient = var_map["eps-gradient"].as<double>();
    if (var_map.count("max-iters") > 0) options.max_iters = var_map["max-iters"].as<size_t>();
    if (var_map.count("max-retries") > 0) options.max_retries = var_map["max-retries"].as<size_t>();
    if (var_map.count("ml-beta") > 0) options.ml_type = 1;
    if (var_map.count("verbose") > 0) options.verbose = var_map["verbose"].as<size_t>();
    if (var_map.count("v1") > 0) options.verbose = 1;
    if (var_map.count("v2") > 0) options.verbose = 2;
    double bw = var_map["bandwidth"].as<double>();
    /// solve
    mat G,X,Z,u;
    vec y;
    uvec group;
    // Read Data
    X.load(arma::csv_name(string(data_dir) + "/hlmgwr_x.csv"));
    G.load(arma::csv_name(string(data_dir) + "/hlmgwr_g.csv"));
    Z.load(arma::csv_name(string(data_dir) + "/hlmgwr_z.csv"));
    u.load(arma::csv_name(string(data_dir) + "/hlmgwr_u.csv"));
    y.load(arma::csv_name(string(data_dir) + "/hlmgwr_y.csv"));
    group.load(arma::csv_name(string(data_dir) + "/hlmgwr_group.csv"));
    HLMGWRArgs alg_args = { G, X, Z, y, u, group, bw };
    HLMGWRParams alg_params = backfitting_maximum_likelihood(alg_args, options);
    // Diagnostic
    const mat &gamma = alg_params.gamma, &beta = alg_params.beta, &mu = alg_params.mu, &D = alg_params.D;
    uword ngroup = G.n_rows, ndata = y.n_rows;
    vec yhat(ndata, arma::fill::zeros);
    for (uword i = 0; i < ngroup; i++)
    {
        uvec ind = find(group == i);
        yhat(ind) = as_scalar(G.row(i) * gamma.row(i).t()) + X.rows(ind) * beta + Z.rows(ind) * mu.row(i).t();
    }
    vec residual = y - yhat;
    vec deviation = y - mean(y);
    double rss = 1 - sum(residual % residual) / sum(deviation % deviation);
    cout << "Rsquared: " << rss << endl;
    // Save coefficients
    alg_params.gamma.save(arma::csv_name(string(data_dir) + "/hlmgwr_hat_gamma.csv"));
    alg_params.beta.save(arma::csv_name(string(data_dir) + "/hlmgwr_hat_beta.csv"));
    alg_params.mu.save(arma::csv_name(string(data_dir) + "/hlmgwr_hat_mu.csv"));
    alg_params.D.save(arma::csv_name(string(data_dir) + "/hlmgwr_hat_D.csv"));
}
