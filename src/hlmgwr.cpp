#include "hlmgwr.h"
#include <sstream>
#include <iomanip>
#include <string>
#include <utility>
#include <gsl/gsl_min.h>
#include <gsl/gsl_multimin.h>
#include <gsl/gsl_errno.h>

using namespace std;
using namespace arma;
using namespace hgwr;

const double log2pi = log(2.0 * M_PI);

double HGWR::bw_criterion_cv(double bw, void* params)
{
    BwSelectionArgs* args = (BwSelectionArgs*)params;
    const mat& Vig = args->Vig.get();
    const vec& Viy = args->Viy.get();
    const mat& G = args->G.get();
    const mat& u = args->u.get();
    const mat* Ygf = args->Ygf;
    const mat* Zf = args->Zf;
    const mat& mu = args->mu.get();
    const size_t ngroup = Viy.n_rows;
    /// Calibrate for each gorup.
    double cv = 0.0;
    for (size_t i = 0; i < ngroup; i++)
    {
        mat d_u = u.each_row() - u.row(i);
        vec d = sqrt(sum(d_u % d_u, 1));
        double b = actual_bw(d, bw);
        vec wW = (*args->kernel)(d % d, b * b);
        wW(i) = 0;
        mat GtWVG = (G.each_col() % wW).t() * Vig;
        mat GtWVy = (G.each_col() % wW).t() * Viy;
        try
        {
            vec bi = solve(GtWVG, GtWVy);
            vec hat_ygi = as_scalar(G.row(i) * bi) + Zf[i] * mu.row(i).t();
            vec residual = Ygf[i] - hat_ygi;
            cv += sum(residual % residual);
        }
        catch(const std::exception& e)
        {
            return DBL_MAX;
        }
    }
    return cv;
}

double HGWR::bw_criterion_aic(double bw, void* params)
{
    BwSelectionArgs* args = (BwSelectionArgs*)params;
    const mat& Vig = args->Vig.get();
    const vec& Viy = args->Viy.get();
    const mat& G = args->G.get();
    const mat& u = args->u.get();
    const mat* Ygf = args->Ygf;
    const mat* Zf = args->Zf;
    const mat& mu = args->mu.get();
    const mat& rVsigma = args->rVsigma.get();
    const uvec& group = args->group.get();
    const size_t ngroup = Viy.n_rows;
    /// Calibrate for each gorup.
    double rss = 0.0;
    double trS = 0.0;
    for (size_t i = 0; i < ngroup; i++)
    {
        mat d_u = u.each_row() - u.row(i);
        vec d = sqrt(sum(d_u % d_u, 1));
        double b = actual_bw(d, bw);
        vec wW = (*args->kernel)(d % d, b * b);
        mat GtW = trans(G.each_col() % wW);
        mat GtWVG = GtW * Vig;
        mat GtWVy = GtW * Viy;
        try
        {
            mat GtWVG_inv = inv(GtWVG);
            vec bi = GtWVG_inv * GtWVy;
            uvec igroup = find(group == i);
            // mat GtWe = GtW.cols(group);
            // mat si = G.rows(group.rows(find(group == i))) * GtWVG_inv * (GtWe.each_row() % rVsigma);
            mat si_left = G.rows(group.rows(igroup)) * GtWVG_inv * GtW.col(i);
            mat si = si_left * rVsigma.cols(igroup);
            trS += trace(si);
            vec hat_ygi = as_scalar(G.row(i) * bi) + Zf[i] * mu.row(i).t();
            vec residual = Ygf[i] - hat_ygi;
            rss += sum(residual % residual);
        }
        catch(const std::exception& e)
        {
            return DBL_MAX;
        }
    }
    double n = double(ngroup);
    double aic = n * log(rss / n) + n * log(2 * arma::datum::pi) + n + trS;
    return aic;
}

int HGWR::bw_optimisation(double lower, double upper, const BwSelectionArgs* args)
{
    gsl_set_error_handler([](const char* reason, const char* file, int line, int gsl_errno)
    {
        (void)reason;
        (void)file;
        (void)line;
        (void)gsl_errno;
    });
    gsl_function func;
    func.params = (void*)args;
    func.function = bw_criterion;
    gsl_min_fminimizer* minimizer = gsl_min_fminimizer_alloc(gsl_min_fminimizer_goldensection);
    const double R = (sqrt(5)-1)/2;
    double m = lower + R * (upper - lower);
    int status = gsl_min_fminimizer_set(minimizer, &func, m, lower, upper);
    if (status == GSL_EINVAL)
    {
        if (bw > 0)
        {
            if (verbose > 0) pcout("Bandwidth optimisation failed. Use last value: " + to_string(bw) + "\n");
        }
        else
        {
            bw = gsl_min_fminimizer_x_minimum(minimizer);
            if (verbose > 0) pcout("Bandwidth optimisation failed to initialise. Use default value: " + to_string(bw) + "\n");
        }
        return GSL_EINVAL;
    }
    size_t iter = 0;
    do
    {
        status = gsl_min_fminimizer_iterate(minimizer);
        m = gsl_min_fminimizer_x_minimum(minimizer);
        lower = gsl_min_fminimizer_x_lower(minimizer);
        upper = gsl_min_fminimizer_x_upper(minimizer);
        status = gsl_min_test_interval(lower, upper, 1e-4, 0.0);
        if (verbose > 1)
        {
            double fm = gsl_min_fminimizer_f_minimum(minimizer);
            pcout(string("xL: ") + to_string(lower) + "; xU: " + to_string(upper) + "; x: " + to_string(m) + "; f: " + to_string(fm) + "\r");
        }
    } while (status == GSL_CONTINUE && iter < max_bw_iters);
    if (status == GSL_SUCCESS)
    {
        bw = m;
        double fm = gsl_min_fminimizer_f_minimum(minimizer);
        if (verbose > 1) pcout("\n");
        if (verbose > 0) pcout(string("bw: ") + to_string(bw) + "; f: " + to_string(fm) + "\n");
    }
    else
    {
        if (verbose > 0) pcout("Bandwidth optimisation failed. Use last value: " + to_string(bw) + "\n");
    }
    gsl_min_fminimizer_free(minimizer);
    gsl_set_error_handler_off();
    return status;
}

/**
 * @brief Estimate $\gamma$.
 * 
 * @param X Equals to $g$
 * @param y Equals to $\bar{y}$
 * @param S Equals to $s$
 * @param u Used to calculate $W$
 * @param bw Bandwidth 
 * @param wn Equals to $N$
 * @param wD Equals to $D$
 * @return mat 
 */
void HGWR::fit_gwr()
{
    uword k = G.n_cols;//, q = Zf[0].n_cols;
    mat D_inv = D.i();
    gamma.fill(arma::fill::zeros);
    mat Vig(ngroup, k, arma::fill::zeros);
    vec Viy(ngroup, arma::fill::zeros);
    vec Yg(ngroup, arma::fill::zeros);
    rowvec rVsigma = rowvec(ndata, arma::fill::zeros);
    for (size_t i = 0; i < ngroup; i++)
    {
        const mat& Yi = Ygf[i];
        const mat& Zi = Zf[i];
        mat Vi_inv = woodbury_eye(D_inv, Zi);
        uword nidata = Zi.n_rows;
        rowvec Visigma = ones(1, nidata) * Vi_inv;
        Vig.row(i) = Visigma * ones(nidata, 1) * G.row(i);
        Viy(i) = as_scalar(Visigma * Yi);
        rVsigma(find(group == i)) = Visigma;
    }
    // mat mVsigma = ones(ngroup, 1) * rVsigma;
    /// Check whether need to optimize bw
    if (bw_optim)
    {
        BwSelectionArgs args { Vig, Viy, G, u, Ygf.get(), Zf.get(), mu, rVsigma, group, gwr_kernel };
        double upper = ngroup - 1, lower = k + 2;
        // bw = golden_selection(lower, upper, true, args);
        bw_optimisation(lower, upper, &args);
    }
    /// Calibrate for each gorup.
    trS = { 0.0, 0.0};
    for (size_t i = 0; i < ngroup; i++)
    {
        mat d_u = u.each_row() - u.row(i);
        vec d = sqrt(sum(d_u % d_u, 1));
        double b = actual_bw(d, bw);
        vec wW = (*gwr_kernel)(d % d, b * b);
        mat GtW = (G.each_col() % wW).t();
        mat GtWVG = GtW * Vig;
        mat GtWVy = GtW * Viy;
        mat GtWVG_inv = inv(GtWVG);
        gamma.row(i) = trans(GtWVG_inv * GtWVy);
        uvec igroup = find(group == i);
        // mat GtWe = GtW.cols(group);
        // mat si = G.rows(group.rows(find(group == i))) * GtWVG_inv * (GtWe.each_row() % rVsigma);
        mat si_left = (G.rows(group.rows(igroup)) * GtWVG_inv * GtW).eval().cols(group);
        mat si = si_left.each_row() % rVsigma;
        trS(0) += trace(si.cols(igroup));
        trS(1) += trace(si * si.t());
    }
}

vec HGWR::fit_gls()
{
    uword p = Xf[0].n_cols;
    mat XtWX(p, p, arma::fill::zeros);
    vec XtWY(p, arma::fill::zeros);
    mat D_inv = D.i();
    for (uword i = 0; i < ngroup; i++)
    {
        const mat& Xi = Xf[i];
        const mat& Yi = Yhf[i];
        const mat& Zi = Zf[i];
        mat Vi_inv = woodbury_eye(D_inv, Zi);
        XtWX += Xi.t() * Vi_inv * Xi;
        XtWY += Xi.t() * Vi_inv * Yi;
    }
    return solve(XtWX, XtWY);
}

double loglikelihood(const mat* Xf, const vec* Yf, const mat* Zf, const size_t ngroup, const mat& D, const vec& beta, const uword& ndata)
{
    mat D_inv = D.i();
    double L1 = 0.0, L2 = 0.0, n = (double)ndata;
    for (uword i = 0; i < ngroup; i++)
    {
        const mat& Xi = Xf[i];
        const vec& Yi = Yf[i];
        const mat& Zi = Zf[i];
        mat Vi = ((Zi * D) * Zi.t()) + eye<mat>(Zi.n_rows, Zi.n_rows);
        double detVi, sign_detVi;
        log_det(detVi, sign_detVi, Vi);
        mat Vi_inv = HGWR::woodbury_eye(D_inv, Zi);
        vec Ri = Yi - Xi * beta;
        L1 += as_scalar(Ri.t() * Vi_inv * Ri);
        L2 += detVi;
    }
    double LL = - (n / 2.0) * log(L1) - 0.5 * L2 - 0.5 - 0.5 * log2pi + (n / 2.0) * log(n);
    return LL;
}

void loglikelihood_d(const mat* Xf, const vec* Yf, const mat* Zf, const size_t ngroup, const mat& D, const vec& beta, const uword& ndata, mat& d_D)
{
    mat ZtViZ(arma::size(D), arma::fill::zeros), D_inv = D.i();
    mat KKt(arma::size(D), arma::fill::zeros);
    double J = 0.0, n = (double)ndata;
    // field<mat> Kf(ngroup);
    for (uword i = 0; i < ngroup; i++)
    {
        const mat& Xi = Xf[i];
        const mat& Yi = Yf[i];
        const mat& Zi = Zf[i];
        mat Vi_inv = HGWR::woodbury_eye(D_inv, Zi);
        vec Ri = Yi - Xi * beta;
        mat Ki = Zi.t() * Vi_inv * Ri;
        KKt += Ki * Ki.t();
        ZtViZ += Zi.t() * Vi_inv * Zi;
        J += as_scalar(Ri.t() * Vi_inv * Ri);
    }
    mat KJKt = KKt / J;
    d_D = ((- n / 2.0) * (-KJKt) - 0.5 * ZtViZ);
}

void loglikelihood_d(const mat* Xf, const vec* Yf, const mat* Zf, const size_t ngroup, const mat& D, const vec& beta, const uword& ndata, mat& d_D, mat& d_beta)
{
    mat ZtViZ(arma::size(D), arma::fill::zeros), D_inv = D.i();
    mat KKt(arma::size(D), arma::fill::zeros), G(arma::size(beta), arma::fill::zeros);
    double J = 0.0, n = (double)ndata;
    // field<mat> Kf(ngroup);
    field<mat> Kf(ngroup), Gf(ngroup);
    for (uword i = 0; i < ngroup; i++)
    {
        const mat& Xi = Xf[i];
        const mat& Yi = Yf[i];
        const mat& Zi = Zf[i];
        mat Vi_inv = HGWR::woodbury_eye(D_inv, Zi);
        vec Ri = Yi - Xi * beta;
        mat Ki = Zi.t() * Vi_inv * Ri;
        KKt += Ki * Ki.t();
        G += Xi.t() * Vi_inv * Ri;
        ZtViZ += Zi.t() * Vi_inv * Zi;
        J += as_scalar(Ri.t() * Vi_inv * Ri);
    }
    mat KJKt = KKt / J;
    mat GJ = G / J;
    d_D = ((- n / 2.0) * (-KJKt) - 0.5 * ZtViZ);
    d_beta = n * GJ;
}

double ml_gsl_f_D(const gsl_vector* v, void* p)
{
    ML_Params* params = (ML_Params*)p;
    const mat* Xf = params->Xf;
    const vec* Yf = params->Yf;
    const mat* Zf = params->Zf;
    const vec* beta = params->beta;
    const size_t ngroup = params->ngroup;
    const uword n = params->n;
    const uword q = params->q;
    size_t ntarget = q * (q + 1) / 2;
    vec D_tri(ntarget, arma::fill::zeros);
    for (size_t i = 0; i < ntarget; i++)
    {
        D_tri(i) = gsl_vector_get(v, i);
    }
    mat D(q, q, arma::fill::zeros);
    D(trimatl_ind(size(D))) = D_tri;
    D = D.t();
    D(trimatl_ind(size(D))) = D_tri;
    double logL = loglikelihood(Xf, Yf, Zf, ngroup, D, *beta, n);
    return -logL / double(n);
}

double ml_gsl_f_D_beta(const gsl_vector* v, void* pparams)
{
    ML_Params* params = (ML_Params*)pparams;
    const mat* Xf = params->Xf;
    const vec* Yf = params->Yf;
    const mat* Zf = params->Zf;
    const size_t ngroup = params->ngroup;
    const uword n = params->n;
    const uword p = params->p;
    const uword q = params->q;
    size_t ntarget = p + q * (q + 1) / 2;
    vec D_tri(q * (q + 1) / 2, arma::fill::zeros), beta(p, arma::fill::zeros);
    for (size_t i = 0; i < p; i++)
    {
        beta(i) = gsl_vector_get(v, i);
    }
    for (size_t i = p; i < ntarget; i++)
    {
        D_tri(i - p) = gsl_vector_get(v, i);
    }
    mat D(q, q, arma::fill::zeros);
    D(trimatl_ind(size(D))) = D_tri;
    D = D.t();
    D(trimatl_ind(size(D))) = D_tri;
    double logL = loglikelihood(Xf, Yf, Zf, ngroup, D, beta, n);
    return -logL / double(n);
}

void ml_gsl_df_D(const gsl_vector* v, void* p, gsl_vector *df)
{
    ML_Params* params = (ML_Params*)p;
    const mat* Xf = params->Xf;
    const vec* Yf = params->Yf;
    const mat* Zf = params->Zf;
    const vec* beta = params->beta;
    const size_t ngroup = params->ngroup;
    const uword n = params->n;
    const uword q = params->q;
    size_t ntarget = q * (q + 1) / 2;
    vec D_tri(ntarget, arma::fill::zeros);
    for (size_t i = 0; i < ntarget; i++)
    {
        D_tri(i) = gsl_vector_get(v, i);
    }
    mat D(q, q, arma::fill::zeros);
    D(trimatl_ind(size(D))) = D_tri;
    D = D.t();
    D(trimatl_ind(size(D))) = D_tri;
    mat dL_D;
    loglikelihood_d(Xf, Yf, Zf, ngroup, D, *beta, n, dL_D);
    dL_D = -dL_D / double(n);
    vec dL_D_tri = dL_D(trimatl_ind(size(D)));
    for (uword i = 0; i < ntarget; i++)
    {
        gsl_vector_set(df, i, dL_D(i));
    }
}

void ml_gsl_df_D_beta(const gsl_vector* v, void* pparams, gsl_vector *df)
{
    ML_Params* params = (ML_Params*)pparams;
    const mat* Xf = params->Xf;
    const vec* Yf = params->Yf;
    const mat* Zf = params->Zf;
    const size_t ngroup = params->ngroup;
    const uword n = params->n;
    const uword p = params->p;
    const uword q = params->q;
    size_t ntarget = p + q * (q + 1) / 2;
    vec D_tri(q * (q + 1) / 2, arma::fill::zeros), beta(p, arma::fill::zeros);
    for (size_t i = 0; i < p; i++)
    {
        beta(i) = gsl_vector_get(v, i);
    }
    for (size_t i = p; i < ntarget; i++)
    {
        D_tri(i - p) = gsl_vector_get(v, i);
    }
    mat D(q, q, arma::fill::zeros);
    D(trimatl_ind(size(D))) = D_tri;
    D = D.t();
    D(trimatl_ind(size(D))) = D_tri;
    mat dL_D;
    vec dL_beta;
    loglikelihood_d(Xf, Yf, Zf, ngroup, D, beta, n, dL_D, dL_beta);
    dL_D = -dL_D / double(n);
    dL_beta = -dL_beta / double(n);
    vec dL_D_tri = dL_D(trimatl_ind(size(D)));
    for (size_t i = 0; i < p; i++)
    {
        gsl_vector_set(df, i, dL_beta(i));
    }
    for (uword i = p; i < ntarget; i++)
    {
        gsl_vector_set(df, i, dL_D_tri(i - p));
    }
}

void ml_gsl_fdf_D(const gsl_vector* v, void* p, double *f, gsl_vector *df)
{
    *f = ml_gsl_f_D(v, p);
    ml_gsl_df_D(v, p, df);
}

void ml_gsl_fdf_D_beta(const gsl_vector* v, void* p, double *f, gsl_vector *df)
{
    *f = ml_gsl_f_D(v, p);
    ml_gsl_df_D_beta(v, p, df);
}

double HGWR::fit_D(ML_Params* params)
{
    int precision = int(log10(1.0 / eps_gradient));
    uword q = D.n_cols, ntarget = q * (q + 1) / 2;
    gsl_multimin_function_fdf minex_fun;
    minex_fun.n = ntarget;
    minex_fun.f = ml_gsl_f_D;
    minex_fun.df = ml_gsl_df_D;
    minex_fun.fdf = ml_gsl_fdf_D;
    minex_fun.params = (void*)params;
    gsl_vector *target = gsl_vector_alloc(ntarget), *step_size = gsl_vector_alloc(ntarget);
    uvec D_tril_idx = trimatl_ind(arma::size(D)), D_triu_idx = trimatu_ind(arma::size(D));
    vec D_tril_vec = D(D_tril_idx);
    for (uword i = 0; i < ntarget; i++)
    {
        gsl_vector_set(target, i, D_tril_vec(i));
        gsl_vector_set(step_size, i, alpha);
    }
    gsl_vector *x0 = gsl_vector_alloc(ntarget);
    gsl_vector_memcpy(x0, target);
    gsl_multimin_fdfminimizer *minimizer = gsl_multimin_fdfminimizer_alloc(gsl_multimin_fdfminimizer_conjugate_fr, ntarget);
    gsl_multimin_fdfminimizer_set(minimizer, &minex_fun, target, alpha, eps_gradient);
    if (verbose > 1)
    {
        ostringstream sout;
        sout << setprecision(precision) << fixed << minimizer->x->data[0];
        for (size_t i = 1; i < D_tril_vec.n_elem; i++)
        {
            sout << "," << minimizer->x->data[i];
        }
        sout << ";";
        sout << setprecision(precision) << fixed << minimizer->gradient->data[0] << ",";
        for (size_t i = 1; i < D_tril_vec.n_elem; i++)
        {
            sout << "," << minimizer->gradient->data[i];
        }
        sout << ";";
        sout << minimizer->f << '\r';
        pcout(sout.str());
    }
    size_t iter = 0;
    int status;
    do
    {
        gsl_vector_memcpy(x0, minimizer->x);
        status = gsl_multimin_fdfminimizer_iterate(minimizer);
        if (verbose > 1)
        {
            ostringstream sout;
            sout << setprecision(precision) << fixed << minimizer->x->data[0];
            for (size_t i = 1; i < D_tril_vec.n_elem; i++)
            {
                sout << "," << minimizer->x->data[i];
            }
            sout << ";";
            sout << setprecision(precision) << fixed << minimizer->gradient->data[0];
            for (size_t i = 1; i < D_tril_vec.n_elem; i++)
            {
                sout << "," << minimizer->gradient->data[i];
            }
            sout << ";";
            sout << minimizer->f << '\r';
            pcout(sout.str());
        }
        if (status || gsl_isnan(minimizer->f)) break;
        status = gsl_multimin_test_gradient(minimizer->gradient, eps_gradient);
    } while (status == GSL_CONTINUE && (++iter) < max_iters);
    if (verbose > 1)
    {
        pcout("\n");
    }
    if (!gsl_isnan(minimizer->f))
    {
        mat D1 = mat(arma::size(D), arma::fill::eye);
        vec D_tri(arma::size(D_tril_idx));
        for (uword i = 0; i < ntarget; i++)
        {
            D_tri(i) = gsl_vector_get(minimizer->x, i);
        }
        D1(D_tril_idx) = D_tri;
        D1 = D1.t();
        D1(D_tril_idx) = D_tri;
        D = D1;
    }
    return minimizer->f;
}

double HGWR::fit_D_beta(ML_Params* params)
{
    int precision = int(log10(1.0 / eps_gradient));
    uword p = beta.n_rows, q = D.n_cols, ntarget = p + q * (q + 1) / 2;
    gsl_multimin_function_fdf minex_fun;
    minex_fun.n = ntarget;
    minex_fun.f = ml_gsl_f_D_beta;
    minex_fun.df = ml_gsl_df_D_beta;
    minex_fun.fdf = ml_gsl_fdf_D_beta;
    minex_fun.params = (void*)params;
    gsl_vector *target = gsl_vector_alloc(ntarget);//, *step_size = gsl_vector_alloc(ntarget);
    for (uword i = 0; i < p; i++)
    {
        gsl_vector_set(target, i, beta(i));
    }
    uvec D_tril_idx = trimatl_ind(arma::size(D)), D_triu_idx = trimatu_ind(arma::size(D));
    vec D_tril_vec = D(D_tril_idx);
    for (uword i = p; i < ntarget; i++)
    {
        uword e = i - p;
        gsl_vector_set(target, i, D_tril_vec(e));
    }
    gsl_vector *x0 = gsl_vector_alloc(ntarget);
    gsl_vector_memcpy(x0, target);
    gsl_multimin_fdfminimizer *minimizer = gsl_multimin_fdfminimizer_alloc(gsl_multimin_fdfminimizer_conjugate_fr, ntarget);
    gsl_multimin_fdfminimizer_set(minimizer, &minex_fun, target, alpha, eps_gradient);
    if (verbose > 1)
    {
        ostringstream sout;
        sout << setprecision(precision) << fixed;
        for (size_t i = 0; i < p; i++)
        {
            sout << minimizer->x->data[i] << ",";
        }
        sout << minimizer->x->data[p];
        for (size_t i = 1; i < D_tril_vec.n_elem; i++)
        {
            sout << "," << minimizer->x->data[p + i];
        }
        sout << ";";
        sout << setprecision(precision) << fixed;
        for (size_t i = 0; i < p; i++)
        {
            sout << minimizer->gradient->data[i] << ",";
        }
        sout << minimizer->x->data[p];
        for (size_t i = 1; i < D_tril_vec.n_elem; i++)
        {
            sout << "," << minimizer->gradient->data[p + i];
        }
        sout << ";"; 
        sout << minimizer->f << '\r';
        pcout(sout.str());
    }
    size_t iter = 0;
    int status;
    do
    {
        gsl_vector_memcpy(x0, minimizer->x);
        status = gsl_multimin_fdfminimizer_iterate(minimizer);
        if (verbose > 1)
        {
            ostringstream sout;
            sout << setprecision(precision) << fixed;
            for (size_t i = 0; i < p; i++)
            {
                sout << minimizer->x->data[i] << ",";
            }
            sout << minimizer->x->data[p];
            for (size_t i = 1; i < D_tril_vec.n_elem; i++)
            {
                sout << "," << minimizer->x->data[p + i];
            }
            sout << ";";
            sout << setprecision(precision) << fixed;
            for (size_t i = 0; i < p; i++)
            {
                sout << minimizer->gradient->data[i] << ",";
            }
            sout << minimizer->x->data[p];
            for (size_t i = 1; i < D_tril_vec.n_elem; i++)
            {
                sout << "," << minimizer->gradient->data[p + i];
            }
            sout << ";"; 
            sout << minimizer->f << '\r';
            pcout(sout.str());
        }
        if (status || gsl_isnan(minimizer->f)) break;
        status = gsl_multimin_test_gradient(minimizer->gradient, eps_gradient);
    } while (status == GSL_CONTINUE && (++iter) < max_iters);
    if (verbose > 1)
    {
        pcout("\n");
    }
    mat D1(arma::size(D), arma::fill::eye);
    vec beta1(arma::size(beta), arma::fill::ones);
    if (!gsl_isnan(minimizer->f))
    {
        vec D_tri(arma::size(D_tril_idx));
        for (uword i = p; i < ntarget; i++)
        {
            D_tri(i - p) = gsl_vector_get(minimizer->x, i);
        }
        D1(D_tril_idx) = D_tri;
        D1 = D1.t();
        D1(D_tril_idx) = D_tri;
        for (uword i = 0; i < p; i++)
        {
            beta1(i) = gsl_vector_get(minimizer->x, i);
        }
    }
    D = D1;
    beta = beta1;
    return minimizer->f;
}

void HGWR::fit_mu()
{
    mat D_inv = D.i();
    mu.fill(arma::fill::zeros);
    for (uword i = 0; i < ngroup; i++)
    {
        const mat& Xi = Xf[i];
        const mat& Yi = Yhf[i];
        const mat& Zi = Zf[i];
        uword ndata = Zi.n_rows;
        mat Vi = Zi * D * Zi.t() + eye(ndata, ndata);
        mat Vi_inv = woodbury_eye(D_inv, Zi);
        vec Ri = Yi - Xi * beta;
        mu.row(i) = (D * Zi.t() * Vi_inv * Ri).t();
    }
}

double HGWR::fit_sigma()
{
    mat D_inv = D.i();
    double sigma2 = 0.0;
    for (uword i = 0; i < ngroup; i++)
    {
        const mat& Xi = Xf[i];
        const mat& Yi = Yhf[i];
        const mat& Zi = Zf[i];
        uword ndata = Zi.n_rows;
        mat Vi = Zi * D * Zi.t() + eye(ndata, ndata);
        mat Vi_inv = woodbury_eye(D_inv, Zi);
        mat Ri = Yi - Xi * beta;
        sigma2 += as_scalar(Ri.t() * Vi_inv * Ri);
    }
    return sqrt(sigma2 / (double)ndata);
}

HGWR::Parameters HGWR::fit()
{
    //===============
    // Prepare Matrix
    //===============
    int prescition = (int)log10(1 / eps_iter);
    double tss = sum((y - mean(y)) % (y - mean(y)));
    gamma = mat(ngroup, nvg, arma::fill::zeros);
    beta = vec(nvx, arma::fill::zeros);
    mu = mat(ngroup, nvz, arma::fill::zeros);
    D = mat(nvz, nvz, arma::fill::eye);
    Zf = make_unique<arma::mat[]>(ngroup);
    Xf = make_unique<arma::mat[]>(ngroup);
    Yf = make_unique<arma::vec[]>(ngroup);
    Ygf = make_unique<arma::vec[]>(ngroup);
    Yhf = make_unique<arma::vec[]>(ngroup);
    for (uword i = 0; i < ngroup; i++)
    {
        uvec ind = find(group == i);
        Yf[i] = y.rows(ind);
        Xf[i] = X.rows(ind);
        Zf[i] = Z.rows(ind);
        Yhf[i] = y.rows(ind);
    }
    //----------------------------------------------
    // Generalized Least Squared Estimation for beta
    //----------------------------------------------
    beta = fit_gls();
    fit_mu();
    //============
    // Backfitting
    //============
    size_t retry = 0;
    double rss = DBL_MAX, rss0 = DBL_MAX, diff = DBL_MAX, mlf = 0.0;
    for (size_t iter = 0; (abs(diff) > eps_iter) && iter < max_iters && retry < max_retries; iter++)
    {
        rss0 = rss;
        //--------------------
        // Initial Guess for M
        //--------------------
        for (uword i = 0; i < ngroup; i++)
        {
            Ygf[i] = Yf[i] - Xf[i] * beta;
        }
        fit_gwr();
        vec hatMg = sum(G % gamma, 1);
        vec hatM = hatMg.rows(group);
        vec yh = y - hatM;
        for (uword i = 0; i < ngroup; i++)
        {
            Yhf[i] = Yf[i] - sum(G.row(i) % gamma.row(i));
        }
        //------------------------------------
        // Maximum Likelihood Estimation for D
        //------------------------------------
        ML_Params ml_params = { Xf.get(), Yhf.get(), Zf.get(), &beta, ngroup, ndata, nvx, nvz };
        switch (ml_type)
        {
        case 1:
            ml_params.beta = nullptr;
            beta = fit_gls();
            mlf = fit_D_beta(&ml_params);
            break;
        default:
            mlf = fit_D(&ml_params);
            beta = fit_gls();
            break;
        }
        fit_mu();
        //------------------------------
        // Calculate Termination Measure
        //------------------------------
        vec yhat = yh - (X * beta) - sum(Z % (mu.rows(group)), 1);
        vec residual = yhat % yhat;
        rss = sum(residual);
        diff = rss - rss0;
        if (rss < rss0) 
        {
            if (retry > 0) retry = 0;
        }
        else if (iter > 0) retry++;
        if (verbose > 0)
        {
            ostringstream sout;
            sout << fixed << setprecision(prescition) << "Iter: " << iter;
            if (bw_optim) sout << ", " << "Bw: " << bw;
            sout << ", " << "RSS: " << rss;
            if (abs(diff) < DBL_MAX) sout << ", " << "dRSS: " << diff;
            sout << ", " << "R2: " << (1 - rss / tss);
            sout << ", " << "-loglik/n: " << mlf;
            if (retry > 0) sout << ", " << "Retry: " << retry;
            sout << endl;
            pcout(sout.str());
        }
    }
    sigma = fit_sigma();
    //============
    // Diagnostic
    //============
    loglik = - mlf * double(ndata);
    calc_var_beta();
    enp = 2 * trS(0) - trS(1);
    edf = ndata - enp;
    return { gamma, beta, mu, D, sigma, bw };
}

void HGWR::calc_var_beta()
{
    mat D_inv = D.i(), XtViX(X.n_cols, X.n_cols, arma::fill::zeros);
    for (uword i = 0; i < ngroup; i++)
    {
        const mat& Xi = Xf[i];
        const mat& Zi = Zf[i];
        uword ndata = Zi.n_rows;
        mat Vi = Zi * D * Zi.t() + eye(ndata, ndata);
        mat Vi_inv = woodbury_eye(D_inv, Zi);
        XtViX += Xi.t() * Vi_inv * Xi;
    }
    var_beta = diagvec(XtViX.i());
}
