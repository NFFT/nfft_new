/*
 * Copyright (c) 2002, 2016 Jens Keiner, Stefan Kunis, Daniel Potts
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */
#include "config.h"

#ifdef HAVE_COMPLEX_H
#include <complex.h>
#endif
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "nfft3.h"
#include "infft.h"
#include "imex.h"
#include "fastsum.h"
#include "kernels.h"

#ifdef HAVE_MEXVERSION_C
  #include "mexversion.c"
#endif

#define PLANS_MAX 100 /* maximum number of plans */
#define CMD_LEN_MAX 40 /* maximum length of command argument */

/* global flags */
#define FASTSUM_MEX_FIRST_CALL (1U << 0)
unsigned short gflags = FASTSUM_MEX_FIRST_CALL;

fastsum_plan* plans[PLANS_MAX]; /* plans */

static inline void check_nargs(const int nrhs, const int n, const char* errmsg)
{
  DM(if (nrhs != n)
    mexErrMsgTxt(errmsg);)
}

static inline void check_plan(int i)
{
  DM(if (i < 0 || i >= PLANS_MAX)
    mexErrMsgTxt("Invalid plan");)
  DM(if (plans[i] == 0)
    mexErrMsgTxt("Plan was not initialized or has already been finalized");)
}

static inline int mkplan()
{
  int i = 0;
  while (i < PLANS_MAX && plans[i] != 0) i++;
  if (i == PLANS_MAX)
    mexErrMsgTxt("fastsum: Too many plans already allocated.");
  plans[i] = nfft_malloc(sizeof(fastsum_plan));
  return i;
}

/* cleanup on mex function unload */
static void cleanup(void)
{
  int i;

  if (!(gflags & FASTSUM_MEX_FIRST_CALL))
  {
    for (i = 0; i < PLANS_MAX; i++)
      if (plans[i])
      {
		nfft_free(plans[i]->kernel_param);
        fastsum_finalize(plans[i]);
        nfft_free(plans[i]);
        plans[i] = 0;
      }
    gflags |= FASTSUM_MEX_FIRST_CALL;
  }
}

void mexFunction(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[])
{
  char cmd[CMD_LEN_MAX];
  if (gflags & FASTSUM_MEX_FIRST_CALL)
  {
    /* Force Matlab to load libfftw3. There is at least one version of Matlab
     * which otherwise crashes upon invocation of this mex function. */
    mexEvalString("fft([1,2,3,4]);");

    nfft_mex_install_mem_hooks();

    /* plan pointers to zeros */
    {
      int i;
      for (i = 0; i < PLANS_MAX; i++)
        plans[i] = 0;
    }

    mexAtExit(cleanup);
    gflags &= ~FASTSUM_MEX_FIRST_CALL;
  }

  /* command string */
  DM(if (nrhs == 0)
    mexErrMsgTxt("At least one input required.");)

  DM(if (!mxIsChar(prhs[0]))
    mexErrMsgTxt("First argument must be a string.");)

  if (mxGetString(prhs[0], cmd, CMD_LEN_MAX))
    mexErrMsgTxt("Could not get command string.");

  if(strcmp(cmd,"get_num_threads") == 0)
  {
    int32_t nthreads = X(get_num_threads)();
    plhs[0] = mxCreateNumericMatrix(1, 1, mxINT32_CLASS, mxREAL);
    *((int32_t *)mxGetData(plhs[0])) = nthreads;

    return;
  }
  else if (strcmp(cmd,"init_guru") == 0)
  {
	check_nargs(nrhs,11,"Wrong number of arguments for init_guru.");
	{
    int i;
	
	int d; /**< number of dimensions    */
	int N; /**< number of source nodes  */
	int M; /**< number of target nodes  */
	int n; /**< expansion degree        */
	int m; /**< cut-off parameter       */
	int p; /**< degree of smoothness    */
	char s[CMD_LEN_MAX+1]; /**< name of kernel          */
	kernel ker; /**< kernel function         */
	double *param; /**< parameter for kernel    */
	double eps_I; /**< inner boundary          */
	double eps_B; /**< outer boundary          */
	param = nfft_malloc(sizeof(double));
	
    d = nfft_mex_get_int(prhs[1],"fastsum: Input argument d must be a scalar.");
	DM(if (d < 1)
		mexErrMsgTxt("nfft: Input argument d must be positive.");)
	N = nfft_mex_get_int(prhs[2],"fastsum: Input argument N must be a scalar.");
	DM(if (N < 1)
		mexErrMsgTxt("nfft: Input argument N must be positive.");)
	M = nfft_mex_get_int(prhs[3],"fastsum: Input argument M must be a scalar.");
	DM(if (M < 1)
		mexErrMsgTxt("nfft: Input argument M must be positive.");)
    n = nfft_mex_get_int(prhs[4],"fastsum: Input argument n must be a scalar.");
	DM(if (n < 1)
		mexErrMsgTxt("nfft: Input argument n must be positive.");)
    m = nfft_mex_get_int(prhs[5],"fastsum: Input argument m must be a scalar.");
	DM(if (m < 1)
		mexErrMsgTxt("nfft: Input argument m must be positive.");)
    p = nfft_mex_get_int(prhs[6],"fastsum: Input argument p must be a scalar.");
	DM(if (p < 1)
		mexErrMsgTxt("nfft: Input argument p must be positive.");)
	if (mxGetString(prhs[7], s, CMD_LEN_MAX))
		mexErrMsgTxt("Could not get kernel string.");
    if (strcmp(s, "gaussian") == 0)
      ker = gaussian;
    else if (strcmp(s, "multiquadric") == 0)
      ker = multiquadric;
    else if (strcmp(s, "inverse_multiquadric") == 0)
      ker = inverse_multiquadric;
    else if (strcmp(s, "logarithm") == 0)
      ker = logarithm;
    else if (strcmp(s, "thinplate_spline") == 0)
      ker = thinplate_spline;
    else if (strcmp(s, "one_over_square") == 0)
      ker = one_over_square;
    else if (strcmp(s, "one_over_modulus") == 0)
      ker = one_over_modulus;
    else if (strcmp(s, "one_over_x") == 0)
      ker = one_over_x;
    else if (strcmp(s, "inverse_multiquadric3") == 0)
      ker = inverse_multiquadric3;
    else if (strcmp(s, "sinc_kernel") == 0)
      ker = sinc_kernel;
    else if (strcmp(s, "cosc") == 0)
      ker = cosc;
    else if (strcmp(s, "cot") == 0)
      ker = kcot;
    else
    {
      mexErrMsgTxt("fastsum: Unknown kernel function.");
    }
    *param = nfft_mex_get_double(prhs[8],"fastsum: Input argument c must be a scalar.");
    eps_I = nfft_mex_get_double(prhs[9],"fastsum: Input argument eps_I must be a scalar.");
    eps_B = nfft_mex_get_double(prhs[10],"fastsum: Input argument eps_B must be a scalar.");

    i = mkplan();
    
	fastsum_init_guru(plans[i], d, N, M, ker, param, 0, n, m, p, eps_I, eps_B);

    plhs[0] = mxCreateDoubleScalar((double)i);
	}
    return;
  }
  
  else if (strcmp(cmd,"set_x") == 0)
  {
    check_nargs(nrhs,3,"Wrong number of arguments for set_x.");
    {
      const int i = nfft_mex_get_int(prhs[1],"fastsum set_x: Input argument plan must be a scalar.");
	  check_plan(i);
      const int N = plans[i]->N_total;
      const int d = plans[i]->d;
      DM(if (!mxIsDouble(prhs[2]) || mxGetNumberOfDimensions(prhs[2]) > 2)
        mexErrMsgTxt("Input argument x must be a N x d double array");)
      DM(if (mxGetM(prhs[2]) != (unsigned)N || mxGetN(prhs[2]) != (unsigned)d)
        mexErrMsgTxt("Input argument x must have correct size.");)
      {
        double *x = mxGetPr(prhs[2]);
		DM(double norm_max = (.25-(plans[i]->eps_B)*.5)*(.25-(plans[i]->eps_B)*.5);)
        for (int k = 0; k < N; k++)
        {
			DM(double norm = 0;)
			for (int t = 0; t < d; t++)
			{
			plans[i]->x[k*d+t] = x[k+t*N];
			DM(norm += plans[i]->x[k*d+t] * plans[i]->x[k*d+t];)
			}
			DM(if( norm > norm_max)
				mexErrMsgTxt("x must be in ball with radius 1/4-eps_B/2");)
        }
      }
    }
    return;
  }
  
  else if (strcmp(cmd,"set_y") == 0)
  {
    check_nargs(nrhs,3,"Wrong number of arguments for set_y.");
    {
      const int i = nfft_mex_get_int(prhs[1],"fastsum set_y: Input argument plan must be a scalar.");
	  check_plan(i);
      const int M = plans[i]->M_total;
      const int d = plans[i]->d;
      DM(if (!mxIsDouble(prhs[2]) || mxGetNumberOfDimensions(prhs[2]) > 2)
        mexErrMsgTxt("Input argument y must be a M x d double array");)
      DM(if (mxGetM(prhs[2]) != (unsigned)M || mxGetN(prhs[2]) != (unsigned)d)
        mexErrMsgTxt("Input argument y must have correct size.");)
      {
        double *y = mxGetPr(prhs[2]);
		DM(double norm_max = (.25-(plans[i]->eps_B)*.5)*(.25-(plans[i]->eps_B)*.5);)
        for (int j = 0; j < M; j++)
        {
			DM(double norm = 0;)
			for (int t = 0; t < d; t++)
			{
			plans[i]->y[d*j+t] = y[j+t*M];
			DM(norm += plans[i]->y[d*j+t] * plans[i]->y[d*j+t];)
			}
			DM(if( norm > norm_max)
				mexErrMsgTxt("x must be in ball with radius 1/4-eps_B/2");)
        }
      }
    }
    return;
  }
  
  else if (strcmp(cmd,"set_alpha") == 0)
  {
    check_nargs(nrhs,3,"Wrong number of arguments for set_alpha.");
    {
      const int i = nfft_mex_get_int(prhs[1],"fastsum set_alpha: Input argument plan must be a scalar.");
	  check_plan(i);
      const int N = plans[i]->N_total;
      DM(if (!mxIsComplex(prhs[2]) || mxGetNumberOfDimensions(prhs[2]) > 2)
        mexErrMsgTxt("Input argument alpha must be a complex N x 1 array");)
      DM(if (mxGetM(prhs[2]) != (unsigned)N || mxGetN(prhs[2]) != 1)
        mexErrMsgTxt("Input argument alpha must have correct size.");)
      {
        double *ar = mxGetPr(prhs[2]), *ai = mxGetPi(prhs[2]);
        for (int k = 0; k < N; k++)
        {
			plans[i]->alpha[k] = ar[k] + _Complex_I*ai[k];
        }
      }
    }
    return;
  }
  
  else if (strcmp(cmd,"trafo_direct") == 0)
  {
    check_nargs(nrhs,2,"Wrong number of arguments for trafo direct.");
    {
      const int i = nfft_mex_get_int(prhs[1],"fastsum: Input argument plan must be a scalar.");
      check_plan(i);
      fastsum_exact(plans[i]);
    }
    return;
  }
  
  else if (strcmp(cmd,"precompute") == 0)
  {
    check_nargs(nrhs,2,"Wrong number of arguments for precompute.");
    {
      const int i = nfft_mex_get_int(prhs[1],"fastsum: Input argument plan must be a scalar.");
      check_plan(i);
      fastsum_precompute(plans[i]);
    }
    return;
  }
  
  else if (strcmp(cmd,"trafo") == 0)
  {
    check_nargs(nrhs,2,"Wrong number of arguments for trafo.");
    {
      const int i = nfft_mex_get_int(prhs[1],"fastsum: Input argument plan must be a scalar.");
      check_plan(i);
      fastsum_trafo(plans[i]);
    }
    return;
  }
  
  else if (strcmp(cmd,"get_f") == 0)
  {
    check_nargs(nrhs,2,"Wrong number of arguments for get_f.");
    {
      const int i = nfft_mex_get_int(prhs[1],"fastsum: Input argument plan must be a scalar.");
      check_plan(i);
      const int M = plans[i]->M_total;
      plhs[0] = mxCreateDoubleMatrix((unsigned int)M, 1, mxCOMPLEX);
      {
        double *fr = mxGetPr(plhs[0]), *fi = mxGetPi(plhs[0]);
        for (int j = 0; j < M; j++)
        {
          fr[j] = creal(plans[i]->f[j]);
          fi[j] = cimag(plans[i]->f[j]);
        }
      }
    }
    return;
  }
  
  else if (strcmp(cmd,"finalize") == 0)
  {
    check_nargs(nrhs,2,"Wrong number of arguments for finalize.");
    {
      const int i = nfft_mex_get_int(prhs[1],"nfft: Input argument plan must be a scalar.");
      check_plan(i);
	  nfft_free(plans[i]->kernel_param);
      fastsum_finalize(plans[i]);
      nfft_free(plans[i]);
      plans[i] = 0;
    }
    return;
  }
  
  
  // Auxiliary functions
  else if (strcmp(cmd,"get_x") == 0)
  {
    check_nargs(nrhs,2,"Wrong number of arguments for get_x.");
    {
      const int i = nfft_mex_get_int(prhs[1],"fastsum: Input argument plan must be a scalar.");
      check_plan(i);
      const int d = plans[i]->d;
      const int N = plans[i]->N_total;
      plhs[0] = mxCreateDoubleMatrix((unsigned int)N, (unsigned int)d, mxREAL);
      {
		double *x = mxGetPr(plhs[0]);
        for (int j = 0; j < N; j++)
        {
			for (int t = 0; t < d; t++)
			{
			x[j+t*N] = plans[i]->x[d*j+t];
			}
        }
      }
    }
    return;
  }
  
  else if (strcmp(cmd,"get_alpha") == 0)
  {
    check_nargs(nrhs,2,"Wrong number of arguments for get_x.");
    {
      const int i = nfft_mex_get_int(prhs[1],"fastsum: Input argument plan must be a scalar.");
      check_plan(i);
      const int N = plans[i]->N_total;
      plhs[0] = mxCreateDoubleMatrix((unsigned int)N, 1, mxCOMPLEX);
      {
		double *ar = mxGetPr(plhs[0]), *ai = mxGetPi(plhs[0]);
        for (int j = 0; j < N; j++)
        {
			ar[j] = creal(plans[i]->alpha[j]);
			ai[j] = cimag(plans[i]->alpha[j]);
        }
      }
    }
    return;
  }
  
  else if (strcmp(cmd,"get_y") == 0)
  {
    check_nargs(nrhs,2,"Wrong number of arguments for get_y.");
    {
      const int i = nfft_mex_get_int(prhs[1],"fastsum: Input argument plan must be a scalar.");
      check_plan(i);
      const int d = plans[i]->d;
      const int M = plans[i]->M_total;
      plhs[0] = mxCreateDoubleMatrix((unsigned int)M, (unsigned int)d, mxREAL);
      {
		double *y = mxGetPr(plhs[0]);
        for (int j = 0; j < M; j++)
        {
			for (int t = 0; t < d; t++)
			{
			y[j+t*M] = plans[i]->y[d*j+t];
			}
        }
      }
    }
    return;
  }
  
  else if (strcmp(cmd,"get_b") == 0)
  {
    check_nargs(nrhs,2,"Wrong number of arguments for get_y.");
    {
      const int i = nfft_mex_get_int(prhs[1],"fastsum: Input argument plan must be a scalar.");
      check_plan(i);
      const int d = plans[i]->d;
	  size_t dims[d];
	  int n_total = 1;
	  for(int j=0; j<d; j++)
	  {
		  dims[j] = plans[i]->n;
		  n_total *= plans[i]->n;
	  }
      plhs[0] = mxCreateNumericArray((unsigned int)d, dims, mxDOUBLE_CLASS, mxCOMPLEX);
      {
		double *br = mxGetPr(plhs[0]), *bi = mxGetPi(plhs[0]);
        for (int j = 0; j < n_total; j++)
        {
			br[j] = creal(plans[i]->b[j]);
			bi[j] = cimag(plans[i]->b[j]);
        }
      }
    }
    return;
  }
  
  else if (strcmp(cmd,"display") == 0)
  {
    check_nargs(nrhs,2,"Wrong number of arguments for set_f_hat_linear.");
    {
      const int i = nfft_mex_get_int(prhs[1],"nfsft: Input argument plan must be a scalar.");
	  check_plan(i);
      mexPrintf("Plan %d\n",i);
      mexPrintf("  pointer: %p\n",plans[i]);
      mexPrintf("        d: %d\n",plans[i]->d);
      mexPrintf("  N_total: %d\n",plans[i]->N_total);
      mexPrintf("  M_total: %d\n",plans[i]->M_total);
      mexPrintf("        n: %d\n",plans[i]->n);
      mexPrintf("        p: %d\n",plans[i]->p);
      mexPrintf("    eps_I: %f\n",plans[i]->eps_I);
      mexPrintf("    eps_B: %f\n",plans[i]->eps_B);
      mexPrintf("        x: %p\n",plans[i]->x);
      mexPrintf("        y: %p\n",plans[i]->y);
      mexPrintf("    alpha: %p\n",plans[i]->alpha);
      mexPrintf("        f: %p\n",plans[i]->f);
      mexPrintf("   kernel: %p\n",plans[i]->k);
      mexPrintf("   _param: %p\n",plans[i]->kernel_param);
      mexPrintf("  *_param: %f\n",*(plans[i]->kernel_param));
      mexPrintf("    flags: %d\n",plans[i]->flags);
    }
    return;
  }
  
  else
    mexErrMsgTxt("fastsum: Unknown command.\n");
}