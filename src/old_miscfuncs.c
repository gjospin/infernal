/* old_miscfuncs.c
 * EPN, Wed Dec  5 10:35:55 2007
 *
 * Options from Infernal codebase as of revision 2243 that
 * are not used in any circumstance. These are typically not
 * old DP functions which can be found in old_cm_dpsearch.c, 
 * old_cm_dpalign.c and old_cp9_dp.c. 
 * 
 * This code is kept solely for reference, and is not
 * compiled/compilable.
 */

#include "esl_config.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "easel.h"
#include "esl_histogram.h"
#include "esl_stopwatch.h"
#include "esl_vectorops.h"
#include "esl_stats.h"

#include "funcs.h"
#include "structs.h"
  

/***********************************************************************
 * Function: CP9Scan_dispatch()
 * Incept:   EPN, Tue Jan  9 06:28:49 2007
 * 
 * Purpose:  Scan a sequence with a CP9, potentially rescan CP9 hits with CM.
 *
 *           3 possible modes:
 *
 *           Mode 1: Filter mode with default pad 
 *                   (IF cm->search_opts & CM_SEARCH_HMMFILTER and 
 *                      !cm->search_opts & CM_SEARCH_HMMPAD)
 *                   Scan with CP9Forward() to get likely endpoints (j) of 
 *                   hits, for each j do a CP9Backward() from j-W+1..j to get
 *                   the most likely start point i for this j. 
 *                   Set i' = j-W+1 and
 *                       j' = i+W-1. 
 *                   Each i'..j' subsequence is rescanned with the CM.
 * 
 *           Mode 2: Filter mode with user-defined pad 
 *                   (IF cm->search_opts & CM_SEARCH_HMMFILTER and 
 *                       cm->search_opts & CM_SEARCH_HMMPAD)
 *                   Same as mode 1, but i' and j' defined differently:
 *                   Set i' = i - (cm->hmmpad) and
 *                       j' = j + (cm->hmmpad) 
 *                   Each i'..j' subsequence is rescanned with the CM.
 * 
 *           Mode 3: HMM only mode (IF cm->search_opts & CM_SEARCH_HMMONLY)
 *                   Hit boundaries i and j are defined the same as in mode 1, but
 *                   no rescanning with CM takes place. i..j hits are reported 
 *                   (note i' and j' are never calculated).
 * 
 * Args:     
 *           cm         - the covariance model, includes cm->cp9: a CP9 HMM
 *           dsq        - sequence in digitized form
 *           i0         - start of target subsequence (1 for beginning of dsq)
 *           j0         - end of target subsequence (L for end of dsq)
 *           W          - the maximum size of a hit (often cm->W)
 *           cm_cutoff  - minimum CM  score to report 
 *           cp9_cutoff - minimum CP9 score to report (or keep if filtering)
 *           results    - search_results_t to add to, only passed to 
 *                        OldActuallySearchTarget()
 *           doing_cp9_stats- TRUE if we're calc'ing stats for the CP9, in this 
 *                            case we never rescan with CM
 *           ret_flen   - RETURN: subseq len that survived filter
 * Returns:  best_sc, score of maximally scoring end position j 
 */
float
CP9Scan_dispatch(CM_t *cm, ESL_DSQ *dsq, int i0, int j0, int W, float cm_cutoff, 
		 float cp9_cutoff, search_results_t *results, int doing_cp9_stats,
		 int *ret_flen)
{
  int h;
  int i;
  int min_i;
  float best_hmm_sc;
  float best_hmm_fsc;
  float cur_best_hmm_bsc;
  float best_cm_sc;
  int   flen;
  float ffrac;
  int do_collapse;
  int ipad;
  int jpad;
  int padmode;
  search_results_t *fwd_results;
  search_results_t *bwd_results;

  /* check contract */
  if(cm->cp9 == NULL)
    cm_Fail("ERROR in CP9Scan_dispatch(), cm->cp9 is NULL\n");
  if((cm->search_opts & CM_SEARCH_HMMPAD) &&
     (!(cm->search_opts & CM_SEARCH_HMMFILTER)))
     cm_Fail("ERROR in CP9Scan_dispatch(), CM_SEARCH_HMMPAD flag up, but CM_SEARCH_HMMFILTER flag down.\n");
  if(!doing_cp9_stats && (!((cm->search_opts & CM_SEARCH_HMMFILTER) || 
			    (cm->search_opts & CM_SEARCH_HMMONLY))))
    cm_Fail("ERROR in CP9Scan_dispatch(), not doing CP9 stats and neither CM_SEARCH_HMMFILTER nor CM_SEARCH_HMMONLY flag is up.\n");
  if(dsq == NULL)
    cm_Fail("ERROR in CP9Scan_dispatch, dsq is NULL.");

  /*printf("in CP9Scan_dispatch(), i0: %d j0: %d\n", i0, j0);
    printf("cp9_cutoff: %f\n", cp9_cutoff);*/

  best_cm_sc = best_hmm_sc = IMPOSSIBLE;
  /* set up options for RescanFilterSurvivors() if we're filtering */
  if(cm->search_opts & CM_SEARCH_HMMFILTER)
    {
      if(cm->search_opts & CM_SEARCH_HMMPAD) /* mode 2 */
	{
	  padmode = PAD_SUBI_ADDJ;
	  ipad = jpad = cm->hmmpad; /* subtract hmmpad from i, add hmmpad to j */
	}
      else /* mode 1 */
	{
	  padmode = PAD_ADDI_SUBJ;
	  ipad = jpad = W-1; /* subtract W-1 from j, add W-1 to i */
	}
      if(cm->search_opts && CM_SEARCH_HBANDED)
	do_collapse = FALSE;
      else
	do_collapse = TRUE;
    }
  
  /* Scan the (sub)seq w/Forward, getting j end points of hits above cutoff */
  fwd_results = CreateResults(INIT_RESULTS);
  best_hmm_fsc = CP9Forward(cm, dsq, i0, j0, W, cp9_cutoff, NULL, NULL, fwd_results,
			    TRUE,   /* we're scanning */
			    FALSE,  /* we're not ultimately aligning */
			    FALSE,  /* we're not rescanning */
			    TRUE,   /* be memory efficient */
			    NULL);  /* don't want the DP matrix back */
  best_hmm_sc = best_hmm_fsc;

  /* Remove overlapping hits, if we're being greedy */
  if(cm->search_opts & CM_SEARCH_HMMGREEDY) /* resolve overlaps by being greedy */
    {
      assert(i0 == 1); 
      remove_overlapping_hits (fwd_results, i0, j0);
    }

  /* Determine start points (i) of the hits based on Backward scan starting at j,
   * report hits IFF CM_SEARCH_HMMONLY */
  bwd_results = CreateResults(INIT_RESULTS);
  for(h = 0; h < fwd_results->num_results; h++) 
    {
      min_i = (fwd_results->data[h].stop - W + 1) >= 1 ? (fwd_results->data[h].stop - W + 1) : 1;
      cur_best_hmm_bsc = CP9Backward(cm, dsq, min_i, fwd_results->data[h].stop, W, cp9_cutoff, 
				     NULL, /* don't care about score of each posn */
				     &i,   /* set i as the best scoring start point from j-W..j */
				     ((cm->search_opts & CM_SEARCH_HMMONLY) ? results : bwd_results),  
				     TRUE,  /* we're scanning */
				     /*FALSE,*/  /* we're not scanning */
				     FALSE, /* we're not ultimately aligning */
				     FALSE, /* don't rescan */
				     TRUE,  /* be memory efficient */
				     NULL); /* don't want the DP matrix back */
      //FALSE,  /* don't be memory efficient */
      //&bmx); /* give the DP matrix back */
      /* this only works if we've saved the matrices, and didn't do scan mode
       * for both Forward and Backward:
       * debug_check_CP9_FB(fmx, bmx, cm->cp9, cur_best_hmm_bsc, i0, j0, dsq); */
      
      if(cur_best_hmm_bsc > best_hmm_sc) best_hmm_sc = cur_best_hmm_bsc;
      /*printf("cur_best_hmm_bsc: %f\n", cur_best_hmm_bsc);*/
    }	  
  /* Rescan with CM if we're filtering and not doing cp9 stats */
  if(!doing_cp9_stats && (cm->search_opts & CM_SEARCH_HMMFILTER))
    {
      /* Remove overlapping hits, if we're being greedy */
      if(cm->search_opts & CM_SEARCH_HMMGREEDY) 
	{
	  assert(i0 == 1); 
	  remove_overlapping_hits (bwd_results, i0, j0);
	}
      best_cm_sc = RescanFilterSurvivors(cm, dsq, bwd_results, i0, j0, W, 
					 padmode, ipad, jpad, 
					 do_collapse, cm_cutoff, cp9_cutoff, 
					 results, &flen);
      if(flen == 0) ffrac = 100.;
      else ffrac = 1. - (((float) flen) / (((float) (j0-i0+1))));
      /*if(!(cm->search_opts & CM_SEARCH_HMMONLY))
	printf("orig_len: %d flen: %d fraction %6.2f\n", (j0-i0+1), (flen), ffrac);*/
    }
  FreeResults (fwd_results);
  FreeResults (bwd_results);

  /*printf("in CP9Scan_dispatch, returning best_hmm_sc: %f\n", best_hmm_sc);*/
  if(doing_cp9_stats || cm->search_opts & CM_SEARCH_HMMONLY)
    return best_hmm_sc;
  else
    return best_cm_sc;
}

/* Function: CP9ScanPosterior()
 * based on Ian Holmes' hmmer/src/postprob.c::P7EmitterPosterior()
 *
 * Purpose:  Combines Forward and Backward scanning matrices into a posterior
 *           probability matrix. 
 *
 *           The main difference between this function and CP9Posterior()
 *           in hmmband.c is that this func takes matrices from CP9ForwardBackwardScan()
 *           in which parses are allowed to start and end in any residue.
 *           In CP9Posterior(), the matrices are calc'ed in CP9Forward()
 *           and CP9Backward() which force all parses considered to start at posn
 *           1 and end at L. This means here we have to calculate probability
 *           that each residue from 1 to L is contained in any parse prior
 *           to determining the posterior probability it was emitted from
 *           each state.
 * 
 *           For emitters (match and inserts) the 
 *           entries in row i of this matrix are the logs of the posterior 
 *           probabilities of each state emitting symbol i of the sequence. 
 *           For non-emitters the entries in row i of this matrix are the 
 *           logs of the posterior probabilities of each state being 'visited' 
 *           when the last emitted residue in the parse was symbol i of the
 *           sequence. 
 *           The last point distinguishes this function from P7EmitterPosterior() 
 *           which set all posterior values for for non-emitting states to -INFTY.
 *           The caller must allocate space for the matrix, although the
 *           backward matrix can be used instead (overwriting it will not
 *           compromise the algorithm).
 *           
 * Args:     dsq      - sequence in digitized form
 *           i0       - start of target subsequence
 *           j0       - end of target subsequence 
 *           hmm      - the model
 *           forward  - pre-calculated forward matrix
 *           backward - pre-calculated backward matrix
 *           mx       - pre-allocated dynamic programming matrix
 *           
 * Return:   void
 */
void
CP9ScanPosterior(ESL_DSQ *dsq, int i0, int j0,
		     CP9_t *hmm,
		     CP9_MX *fmx,
		     CP9_MX *bmx,
		     CP9_MX *mx)
{
  int i;
  int ip;
  int k;
  int fb_sum; /* tmp value, the probability that the current residue (i) was
	       * visited in any parse */
  /* contract check */
  if(dsq == NULL)
    cm_Fail("ERROR in CP9ScanPosterior(), dsq is NULL.");

  /*printf("\n\nin CP9ScanPosterior() i0: %d, j0: %d\n", i0, j0);*/
  fb_sum = -INFTY;
  for (i = i0-1; i <= j0; i++) 
    {
      ip = i-i0+1; /* ip is relative position in seq, 0..L */
      /*printf("bmx->mmx[i:%d][0]: %d\n", i, bmx->mmx[ip][0]); */
      fb_sum = ILogsum(fb_sum, (bmx->mmx[ip][0])); 
    }
  /*printf("fb_sc: %f\n", Scorify(fb_sum));*/
    /* fb_sum is the probability of all parses */

    /*for(k = 1; k <= hmm->M; k++)*/
  /*{*/
      /*fbsum_ = ILogsum(fmx->mmx[0][k] + bmx->mmx[0][k]))*/; /* residue 0 can't be emitted
								    * but we can start in BEGIN,
								    * before any residues */
      /*fb_sum = ILogsum(fb_sum, (fmx->imx[0][k] + bmx->imx[0][k]))*/; /* these will be all -INFTY */
  /*}*/      

  /* note boundary conditions, case by case by case... */
  mx->mmx[0][0] = fmx->mmx[0][0] + bmx->mmx[0][0] - fb_sum;
  mx->imx[0][0] = -INFTY; /*need seq to get here*/
  mx->dmx[0][0] = -INFTY; /*D_0 does not exist*/
  for (k = 1; k <= hmm->M; k++) 
    {
      mx->mmx[0][k] = -INFTY; /*need seq to get here*/
      mx->imx[0][k] = -INFTY; /*need seq to get here*/
      mx->dmx[0][k] = fmx->dmx[0][k] + bmx->dmx[0][k] - fb_sum;
    }
      
  for (i = i0; i <= j0; i++)
    {
      ip = i-i0+1; /* ip is relative position in seq, 0..L */
      /*fb_sum = -INFTY;*/ /* this will be probability of seeing residue i in any parse */
      /*for (k = 0; k <= hmm->M; k++) 
	{
	fb_sum = ILogsum(fb_sum, (fmx->mmx[i][k] + bmx->mmx[i][k] - hmm->msc[dsq[i]][k]));*/
	  /*hmm->msc[dsq[i]][k] will have been counted in both fmx->mmx and bmx->mmx*/
      /*fb_sum = ILogsum(fb_sum, (fmx->imx[i][k] + bmx->imx[i][k] - hmm->isc[dsq[i]][k]));*/
	  /*hmm->isc[dsq[i]][k] will have been counted in both fmx->imx and bmx->imx*/
      /*}*/
      mx->mmx[ip][0] = -INFTY; /*M_0 does not emit*/
      mx->imx[ip][0] = fmx->imx[ip][0] + bmx->imx[ip][0] - hmm->isc[dsq[i]][0] - fb_sum;
      /*hmm->isc[dsq[i]][0] will have been counted in both fmx->imx and bmx->imx*/
      mx->dmx[ip][0] = -INFTY; /*D_0 does not exist*/
      for (k = 1; k <= hmm->M; k++) 
	{
	  mx->mmx[ip][k] = fmx->mmx[ip][k] + bmx->mmx[ip][k] - hmm->msc[dsq[i]][k] - fb_sum;
	  /*hmm->msc[dsq[i]][k] will have been counted in both fmx->mmx and bmx->mmx*/
	  mx->imx[ip][k] = fmx->imx[ip][k] + bmx->imx[ip][k] - hmm->isc[dsq[i]][k] - fb_sum;
	  /*hmm->isc[dsq[i]][k] will have been counted in both fmx->imx and bmx->imx*/
	  mx->dmx[ip][k] = fmx->dmx[ip][k] + bmx->dmx[ip][k] - fb_sum;
	}	  
    }
  /*
  float temp_sc;
  for (i = i0; i <= j0; i++)
    {
      ip = i-i0+1; 
      for(k = 0; k <= hmm->M; k++)
	{
	  temp_sc = Score2Prob(mx->mmx[ip][k], 1.);
	  if(temp_sc > .0001)
	    printf("mx->mmx[%3d][%3d]: %9d | %8f\n", i, k, mx->mmx[ip][k], temp_sc);
	  temp_sc = Score2Prob(mx->imx[ip][k], 1.);
	  if(temp_sc > .0001)
	    printf("mx->imx[%3d][%3d]: %9d | %8f\n", i, k, mx->imx[ip][k], temp_sc);
	  temp_sc = Score2Prob(mx->dmx[ip][k], 1.);
	  if(temp_sc > .0001)
	    printf("mx->dmx[%3d][%3d]: %9d | %8f\n", i, k, mx->dmx[ip][k], temp_sc);
	}
    }
  */
}

/***********************************************************************
 * Function: CP9Scan_dispatch()
 * Incept:   EPN, Tue Jan  9 06:28:49 2007
 * 
 * Purpose:  Scan a sequence with a CP9, potentially rescan CP9 hits with CM.
 *
 *           3 possible modes:
 *
 *           Mode 1: Filter mode with default pad 
 *                   (IF cm->search_opts & CM_SEARCH_HMMFILTER and 
 *                      !cm->search_opts & CM_SEARCH_HMMPAD)
 *                   Scan with CP9Forward() to get likely endpoints (j) of 
 *                   hits, for each j do a CP9Backward() from j-W+1..j to get
 *                   the most likely start point i for this j. 
 *                   Set i' = j-W+1 and
 *                       j' = i+W-1. 
 *                   Each i'..j' subsequence is rescanned with the CM.
 * 
 *           Mode 2: Filter mode with user-defined pad 
 *                   (IF cm->search_opts & CM_SEARCH_HMMFILTER and 
 *                       cm->search_opts & CM_SEARCH_HMMPAD)
 *                   Same as mode 1, but i' and j' defined differently:
 *                   Set i' = i - (cm->hmmpad) and
 *                       j' = j + (cm->hmmpad) 
 *                   Each i'..j' subsequence is rescanned with the CM.
 * 
 *           Mode 3: HMM only mode (IF cm->search_opts & CM_SEARCH_HMMONLY)
 *                   Hit boundaries i and j are defined the same as in mode 1, but
 *                   no rescanning with CM takes place. i..j hits are reported 
 *                   (note i' and j' are never calculated).
 * 
 * Args:     
 *           cm         - the covariance model, includes cm->cp9: a CP9 HMM
 *           dsq        - sequence in digitized form
 *           i0         - start of target subsequence (1 for beginning of dsq)
 *           j0         - end of target subsequence (L for end of dsq)
 *           W          - the maximum size of a hit (often cm->W)
 *           cm_cutoff  - minimum CM  score to report 
 *           cp9_cutoff - minimum CP9 score to report (or keep if filtering)
 *           results    - search_results_t to add to, only passed to 
 *                        OldActuallySearchTarget()
 *           doing_cp9_stats- TRUE if we're calc'ing stats for the CP9, in this 
 *                            case we never rescan with CM
 *           ret_flen   - RETURN: subseq len that survived filter
 * Returns:  best_sc, score of maximally scoring end position j 
 */
float
CP9Scan_dispatch(CM_t *cm, ESL_DSQ *dsq, int i0, int j0, int W, float cm_cutoff, 
		 float cp9_cutoff, search_results_t *results, int doing_cp9_stats,
		 int *ret_flen)
{
  int h;
  int i;
  int min_i;
  float best_hmm_sc;
  float best_hmm_fsc;
  float cur_best_hmm_bsc;
  float best_cm_sc;
  int   flen;
  float ffrac;
  int do_collapse;
  int ipad;
  int jpad;
  int padmode;
  search_results_t *fwd_results;
  search_results_t *bwd_results;

  /* check contract */
  if(cm->cp9 == NULL)
    cm_Fail("ERROR in CP9Scan_dispatch(), cm->cp9 is NULL\n");
  if((cm->search_opts & CM_SEARCH_HMMPAD) &&
     (!(cm->search_opts & CM_SEARCH_HMMFILTER)))
     cm_Fail("ERROR in CP9Scan_dispatch(), CM_SEARCH_HMMPAD flag up, but CM_SEARCH_HMMFILTER flag down.\n");
  if(!doing_cp9_stats && (!((cm->search_opts & CM_SEARCH_HMMFILTER) || 
			    (cm->search_opts & CM_SEARCH_HMMONLY))))
    cm_Fail("ERROR in CP9Scan_dispatch(), not doing CP9 stats and neither CM_SEARCH_HMMFILTER nor CM_SEARCH_HMMONLY flag is up.\n");
  if(dsq == NULL)
    cm_Fail("ERROR in CP9Scan_dispatch, dsq is NULL.");

  /*printf("in CP9Scan_dispatch(), i0: %d j0: %d\n", i0, j0);
    printf("cp9_cutoff: %f\n", cp9_cutoff);*/

  best_cm_sc = best_hmm_sc = IMPOSSIBLE;
  /* set up options for RescanFilterSurvivors() if we're filtering */
  if(cm->search_opts & CM_SEARCH_HMMFILTER)
    {
      if(cm->search_opts & CM_SEARCH_HMMPAD) /* mode 2 */
	{
	  padmode = PAD_SUBI_ADDJ;
	  ipad = jpad = cm->hmmpad; /* subtract hmmpad from i, add hmmpad to j */
	}
      else /* mode 1 */
	{
	  padmode = PAD_ADDI_SUBJ;
	  ipad = jpad = W-1; /* subtract W-1 from j, add W-1 to i */
	}
      if(cm->search_opts && CM_SEARCH_HBANDED)
	do_collapse = FALSE;
      else
	do_collapse = TRUE;
    }
  
  /* Scan the (sub)seq w/Forward, getting j end points of hits above cutoff */
  fwd_results = CreateResults(INIT_RESULTS);
  best_hmm_fsc = CP9Forward(cm, dsq, i0, j0, W, cp9_cutoff, NULL, NULL, fwd_results,
			    TRUE,   /* we're scanning */
			    FALSE,  /* we're not ultimately aligning */
			    FALSE,  /* we're not rescanning */
			    TRUE,   /* be memory efficient */
			    NULL);  /* don't want the DP matrix back */
  best_hmm_sc = best_hmm_fsc;

  /* Remove overlapping hits, if we're being greedy */
  if(cm->search_opts & CM_SEARCH_HMMGREEDY) /* resolve overlaps by being greedy */
    {
      assert(i0 == 1); 
      remove_overlapping_hits (fwd_results, i0, j0);
    }

  /* Determine start points (i) of the hits based on Backward scan starting at j,
   * report hits IFF CM_SEARCH_HMMONLY */
  bwd_results = CreateResults(INIT_RESULTS);
  for(h = 0; h < fwd_results->num_results; h++) 
    {
      min_i = (fwd_results->data[h].stop - W + 1) >= 1 ? (fwd_results->data[h].stop - W + 1) : 1;
      cur_best_hmm_bsc = CP9Backward(cm, dsq, min_i, fwd_results->data[h].stop, W, cp9_cutoff, 
				     NULL, /* don't care about score of each posn */
				     &i,   /* set i as the best scoring start point from j-W..j */
				     ((cm->search_opts & CM_SEARCH_HMMONLY) ? results : bwd_results),  
				     TRUE,  /* we're scanning */
				     /*FALSE,*/  /* we're not scanning */
				     FALSE, /* we're not ultimately aligning */
				     FALSE, /* don't rescan */
				     TRUE,  /* be memory efficient */
				     NULL); /* don't want the DP matrix back */
      //FALSE,  /* don't be memory efficient */
      //&bmx); /* give the DP matrix back */
      /* this only works if we've saved the matrices, and didn't do scan mode
       * for both Forward and Backward:
       * debug_check_CP9_FB(fmx, bmx, cm->cp9, cur_best_hmm_bsc, i0, j0, dsq); */
      
      if(cur_best_hmm_bsc > best_hmm_sc) best_hmm_sc = cur_best_hmm_bsc;
      /*printf("cur_best_hmm_bsc: %f\n", cur_best_hmm_bsc);*/
    }	  
  /* Rescan with CM if we're filtering and not doing cp9 stats */
  if(!doing_cp9_stats && (cm->search_opts & CM_SEARCH_HMMFILTER))
    {
      /* Remove overlapping hits, if we're being greedy */
      if(cm->search_opts & CM_SEARCH_HMMGREEDY) 
	{
	  assert(i0 == 1); 
	  remove_overlapping_hits (bwd_results, i0, j0);
	}
      best_cm_sc = RescanFilterSurvivors(cm, dsq, bwd_results, i0, j0, W, 
					 padmode, ipad, jpad, 
					 do_collapse, cm_cutoff, cp9_cutoff, 
					 results, &flen);
      if(flen == 0) ffrac = 100.;
      else ffrac = 1. - (((float) flen) / (((float) (j0-i0+1))));
      /*if(!(cm->search_opts & CM_SEARCH_HMMONLY))
	printf("orig_len: %d flen: %d fraction %6.2f\n", (j0-i0+1), (flen), ffrac);*/
    }
  FreeResults (fwd_results);
  FreeResults (bwd_results);

  /*printf("in CP9Scan_dispatch, returning best_hmm_sc: %f\n", best_hmm_sc);*/
  if(doing_cp9_stats || cm->search_opts & CM_SEARCH_HMMONLY)
    return best_hmm_sc;
  else
    return best_cm_sc;
}

/***********************************************************************
 * Function: RescanFilterSurvivors()
 * Incept:   EPN, Wed Apr 11 05:51:55 2007
 * 
 * Purpose:  Given start and end points of hits that have survived
 *           a CP9 filter, pad a given amount of residues on 
 *           on each side, and rescan with a CM. Optionally,
 *           collapse overlapping subseqs into one larger subseq before
 *           rescanning (we don't always do this b/c we may want to
 *           derive HMM bands for a subseq from a Forward/Backward scan).
 * 
 *           Can be run in 2 modes, depending on input variable padmode:
 *           Mode 1: padmode = PAD_SUBI_ADDJ
 *                   For each i,j pair in hiti, hitj: 
 *                   set i' = i - ipad; and j' = j + jpad, 
 *                   Rescan subseq from i' to j'.
 *           Mode 2: padmode = PAD_ADDI_SUBJ
 *                   For each i,j pair in hiti, hitj: 
 *                   set j' = i + ipad; and i' = j - jpad, 
 *                   ensure j' >= j and i' <= i. 
 *                   Rescan subseq from i' to j'.
 *
 * Args:     
 *           cm         - the covariance model, includes cm->cp9: a CP9 HMM
 *           dsq        - sequence in digitized form
 *           hmm_results- info on HMM hits that survived filter
 *           i0         - start of target subsequence (1 for beginning of dsq)
 *           j0         - end of target subsequence (L for end of dsq)
 *           W          - the maximum size of a hit (often cm->W)
 *           padmode    - PAD_SUBI_ADDJ or PAD_ADDI_SUBJ (see above)
 *           ipad       - number of residues to subtract/add from each i 
 *           jpad       - number of residues to add/subtract from each j 
 *           do_collapse- TRUE: collapse overlapping hits (after padding) into 1
 *           cm_cutoff  - minimum CM  score to report 
 *           cp9_cutoff - minimum CP9 score to report 
 *           results    - search_results_t to add to, only passed to 
 *                        OldActuallySearchTarget()
 *           ret_flen   - RETURN: subseq len that survived filter
 * Returns:  best_sc found when rescanning with CM 
 */
float
RescanFilterSurvivors(CM_t *cm, ESL_DSQ *dsq, search_results_t *hmm_results, int i0, int j0,
		      int W, int padmode, int ipad, int jpad, int do_collapse,
		      float cm_cutoff, float cp9_cutoff, search_results_t *results, int *ret_flen)
{
  int h;
  int i, j;
  float best_cm_sc;
  float cm_sc;
  int   flen;
  int   prev_j = j0;
  int   next_j;
  int   nhits;

  /* check contract */
  if(padmode != PAD_SUBI_ADDJ && padmode != PAD_ADDI_SUBJ)
    ESL_EXCEPTION(eslEINCOMPAT, "can't determine mode.");
  if(dsq == NULL)
    cm_Fail("ERROR in RescanFilterSurvivors(), dsq is NULL.\n");

  best_cm_sc = IMPOSSIBLE;
  flen = 0;

  /*if(padmode == PAD_SUBI_ADDJ)
    printf("in RescanFilterSurvivors(), mode: PAD_SUBI_ADDJ\n");
    else
    printf("in RescanFilterSurvivors(), mode: PAD_ADDI_SUBJ\n");
    printf("\tipad: %d, jpad: %d collapse: %d\n", ipad, jpad, do_collapse);*/

  /* For each hit, add pad according to mode and rescan by calling OldActuallySearchTarget(). 
   * If do_collapse, collapse multiple overlapping hits into 1 before rescanning */
  /* hits should always be sorted by decreasing j, if this is violated - die. */
  nhits = hmm_results->num_results;
  for(h = 0; h < nhits; h++) 
    {
      if(hmm_results->data[h].stop > prev_j) 
	ESL_EXCEPTION(eslEINCOMPAT, "j's not in descending order");

      prev_j = hmm_results->data[h].stop;

      /* add pad */
      if(padmode == PAD_SUBI_ADDJ)
	{
	  i = ((hmm_results->data[h].start - ipad) >= 1)    ? (hmm_results->data[h].start - ipad) : 1;
	  j = ((hmm_results->data[h].stop  + jpad) <= j0)   ? (hmm_results->data[h].stop  + jpad) : j0;
	  if((h+1) < nhits)
	    next_j = ((hmm_results->data[h+1].stop + jpad) <= j0)   ? (hmm_results->data[h+1].stop + jpad) : j0;
	  else
	    next_j = -1;
	}
      else if(padmode == PAD_ADDI_SUBJ)
	{
	  i = ((hmm_results->data[h].stop  - jpad) >= 1)    ? (hmm_results->data[h].stop  - jpad) : 1;
	  j = ((hmm_results->data[h].start + ipad) <= j0)   ? (hmm_results->data[h].start + ipad) : j0;
	  if((h+1) < nhits)
	    next_j = ((hmm_results->data[h+1].start + ipad) <= j0)   ? (hmm_results->data[h+1].start + ipad) : j0;
	  else
	    next_j = -1;
	}
      /*printf("subseq: hit %d i: %d (%d) j: %d (%d)\n", h, i, hmm_results->data[h].start[h], j, hmm_results->data[h].stop[h]);*/

      if(do_collapse) /* collapse multiple hits that overlap after padding on both sides into a single hit */
	{
	  while(((h+1) < nhits) && (next_j >= i))
	    {
	      /* suck in hit */
	      h++;
	      if(padmode == PAD_SUBI_ADDJ)
		{
		  i = ((hmm_results->data[h].start - ipad) >= 1)    ? (hmm_results->data[h].start - ipad) : 1;
		  if((h+1) < nhits)
		    next_j = ((hmm_results->data[h+1].stop + jpad) <= j0)   ? (hmm_results->data[h+1].stop + jpad) : j0;
		  else
		    next_j = -1;
		}
	      else if(padmode == PAD_ADDI_SUBJ)
		{
		  i = ((hmm_results->data[h].stop - jpad) >= 1)    ? (hmm_results->data[h].stop - jpad) : 1;
		  if((h+1) < nhits)
		    next_j = ((hmm_results->data[h+1].start + ipad) <= j0)   ? (hmm_results->data[h+1].start + ipad) : j0;
		  else
		    next_j = -1;
		}
	      /*printf("\tsucked in subseq: hit %d new_i: %d j (still): %d\n", h, i, j);*/
	    }
	}
      /*printf("in RescanFilterSurvivors(): calling OldActuallySearchTarget: %d %d h: %d nhits: %d\n", i, j, h, nhits);*/
      cm_sc =
	OldActuallySearchTarget(cm, dsq, i, j, cm_cutoff, cp9_cutoff,
				results, /* keep results                                 */
				FALSE,   /* don't filter, we already have                */
				FALSE,   /* we're not building a histogram for CM stats  */
				FALSE,   /* we're not building a histogram for CP9 stats */
				NULL,    /* filter fraction N/A                          */
				FALSE);  /* DO NOT align the hits in this recursive call */
      flen += (j - i + 1);
      if(cm_sc > best_cm_sc) best_cm_sc = cm_sc;
    }

  //if(flen == 0) ffrac = 100.;
  //else ffrac = 1. - (((float) flen) / (((float) (j0-i0+1))));
  if(ret_flen != NULL) *ret_flen = flen;
  return best_cm_sc;
}


/*
 * Function: FindCP9FilterThreshold()
 * Incept:   EPN, Wed May  2 10:00:45 2007
 *
 * Purpose:  Sample sequences from a CM and determine the CP9 HMM E-value
 *           threshold necessary to recognize a specified fraction of those
 *           hits. Sequences are sampled from the CM until N with a E-value
 *           better than cm_ecutoff are sampled (those with worse E-values
 *           are rejected). CP9 scans are carried out in either local or
 *           glocal mode depending on hmm_gum_mode. CM is configured in 
 *           local/glocal and sampled seqs are scored in CYK/inside depending
 *           on fthr_mode (4 possibilities). E-values are determined using
 *           lambda from cm->stats, and a recalc'ed mu using database size
 *           of 'db_size'.
 *
 *           If do_fastfil and fthr_mode is local or glocal CYK, parsetree 
 *           scores of emitted sequences are assumed to an optimal CYK scores 
 *           (not nec true). This saves a lot of time b/c no need to
 *           scan emitted seqs, but it's statistically wrong. 
 *           If do_fastfil and fthr_mode is local or glocal inside, 
 *           contract is violated and we Die. Current strategy *outside*
 *           of this function is to copy HMM filtering thresholds from
 *           CYK for Inside cases.
 *
 *           If !do_fastfil this function takes much longer b/c emitted 
 *           parsetree score is not necessarily (a) optimal nor (b) highest 
 *           score returned from a CYK scan (a subseq of the full seq could
 *           score higher even if parsetree was optimal). For Inside, Inside
 *           scan will always be higher than parstree score. This means we have
 *           to scan each emitted seq with CYK or Inside.
 *
 * Args:
 *           cm           - the CM
 *           cmstats      - CM stats object we'll get Gumbel stats from
 *           r            - source of randomness for EmitParsetree()
 *           Fmin         - minimum target fraction of CM hits to detect with CP9
 *           Smin         - minimum filter survival fraction, for lower fractions, we'll
 *                          increase F. 
 *           Starget      - target filter survival fraction, for lower fractions that 
 *                          satisfy Fmin, we'll increase F until Starget is reached
 *           Spad         - fraction of (sc(S) - sc(max(Starget, Smin))) to subtract from sc(S)
 *                          in case 2. 0.0 = fast, 1.0 = safe.
 *           N            - number of sequences to sample from CM better than cm_minsc
 *           use_cm_cutoff- TRUE to only accept CM parses w/E-values above cm_ecutoff
 *           cm_ecutoff   - minimum CM E-value to accept 
 *           db_size      - DB size (nt) to use w/cm_ecutoff to calc CM bit score cutoff 
 *           emit_mode    - CM_LC or CM_GC, CM mode to emit with
 *           fthr_mode    - gives CM search strategy to use, and Gumbel to use
 *           hmm_gum_mode - CP9_L to search with  local HMM (we're filling a fthr->l_eval)
 *                          CP9_G to search with glocal HMM (we're filling a fthr->g_eval)
 *           do_fastfil   - TRUE to use fast method: assume parsetree score
 *                          is optimal CYK score
 *           do_Fstep     - TRUE to step from F towards 1.0 while S < Starget in case 2.
 *           my_rank      - MPI rank, 0 if serial
 *           nproc        - number of processors in MPI rank, 1 if serial
 *           do_mpi       - TRUE if we're doing MPI, FALSE if not
 *           histfile     - root string for histogram files, we'll 4 of them
 *           Rpts_fp      - open file ptr for optimal HMM/CM score pts
 *           ret_F        - the fraction of observed CM hits we've scored with the HMM better
 *                          than return value
 * 
 * Returns: HMM E-value cutoff above which the HMM scores (ret_F * N) CM 
 *          hits with CM E-values better than cm_ecutoff 
 * 
 */
float FindCP9FilterThreshold(CM_t *cm, CMStats_t *cmstats, ESL_RANDOMNESS *r, 
			     float Fmin, float Smin, float Starget, float Spad, int N, 
			     int use_cm_cutoff, float cm_ecutoff, int db_size, 
			     int emit_mode, int fthr_mode, int hmm_gum_mode, 
			     int do_fastfil, int do_Fstep, int my_rank, int nproc, int do_mpi, 
			     char *histfile, FILE *Rpts_fp, float *ret_F)
{

  /* Contract checks */
  if (!(cm->flags & CMH_CP9) || cm->cp9 == NULL) 
    cm_Fail("ERROR in FindCP9FilterThreshold() CP9 does not exist\n");
  if (Fmin < 0. || Fmin > 1.)  
    cm_Fail("ERROR in FindCP9FilterThreshold() Fmin is %f, should be [0.0..1.0]\n", Fmin);
  if (Smin < 0. || Smin > 1.)  
    cm_Fail("ERROR in FindCP9FilterThreshold() Smin is %f, should be [0.0..1.0]\n", Smin);
  if (Starget < 0. || Starget > 1.)  
    cm_Fail("ERROR in FindCP9FilterThreshold() Starget is %f, should be [0.0..1.0]\n", Starget);
  if((fthr_mode != CM_LI) && (fthr_mode != CM_GI) && (fthr_mode != CM_LC) && (fthr_mode != CM_GC))
    cm_Fail("ERROR in FindCP9FilterThreshold() fthr_mode not CM_LI, CM_GI, CM_LC, or CM_GC\n");
  if(hmm_gum_mode != CP9_L && hmm_gum_mode != CP9_G)
    cm_Fail("ERROR in FindCP9FilterThreshold() hmm_gum_mode not CP9_L or CP9_G\n");
  if(do_fastfil && (fthr_mode == CM_LI || fthr_mode == CM_GI))
    cm_Fail("ERROR in FindCP9FilterThreshold() do_fastfil TRUE, but fthr_mode CM_GI or CM_LI\n");
  if(my_rank > 0 && !do_mpi)
    cm_Fail("ERROR in FindCP9FilterThreshold() my_rank is not 0, but do_mpi is FALSE\n");
  if(emit_mode != CM_GC && emit_mode != CM_LC)
    cm_Fail("ERROR in FindCP9FilterThreshold() emit_mode not CM_LC or CM_GC\n");
  if(emit_mode == CM_LC && (fthr_mode == CM_GC || fthr_mode == CM_GI))
    cm_Fail("ERROR in FindCP9FilterThreshold() emit_mode CM_LC but score mode CM_GC or CM_GI.\n");
  if(Spad < 0 || Spad > 1.0)
    cm_Fail("ERROR in FindCP9FilterThreshold() Spad %f not between 0.0 and 1.0\n");

#if defined(USE_MPI)  && defined(NOTDEFINED)
  /* If a worker in MPI mode, we go to worker function mpi_worker_cm_and_cp9_search */
  if(my_rank > 0) 
    {
      /* Configure the CM based on the fthr mode COULD BE DIFFERENT FROM MASTER! */
      ConfigForGumbelMode(cm, fthr_mode);
      /* Configure the HMM based on the hmm_gum_mode */
      if(hmm_gum_mode == CP9_L)
	{
	  CPlan9SWConfig(cm->cp9, cm->pbegin, cm->pbegin);
	  CPlan9ELConfig(cm);
	}
      else /* hmm_gum_mode == CP9_G (it's in the contract) */
	CPlan9GlobalConfig(cm->cp9);
      CP9Logoddsify(cm->cp9);

      //mpi_worker_cm_and_cp9_search(cm, do_fastfil, my_rank);
      mpi_worker_cm_and_cp9_search_maxsc(cm, do_fastfil, do_minmax, my_rank);

      *ret_F = 0.0; /* this return value is irrelevant */
      return 0.0;   /* this return value is irrelevant */
    }
#endif 
  int            status;         /* Easel status */
  CM_t          *cm_for_scoring; /* used to score parsetrees, nec b/c  *
				  * emitting mode may != scoring mode   */
  Parsetree_t   *tr = NULL;      /* parsetree                           */
  ESL_SQ        *sq = NULL;      /* digitized sequence                  */
  float         *tr_sc;          /* scores of all parsetrees sampled    */
  float         *cm_sc_p;        /* CM scores of samples above thresh   */
  float         *hmm_sc_p;       /* HMM scores of samples above thresh  */
  float         *hmm_eval_p;     /* HMM E-values of samples above thresh*/  
  int            i  = 0;         /* counter over samples                */
  int            ip = 0;         /* counter over samples above thresh   */
  int            imax = 500 * N; /* max num samples                     */
  int            p;              /* counter over partitions             */
  int            L;              /* length of a sample                  */
  float          F;              /* fraction of hits found by HMM >= Fmin*/
  float          E;              /* HMM CP9 E-value cutoff to return    */
  float          Emin;           /* E-value that corresponds to Smin */
  float          Etarget;        /* E-value that corresponds to Starget */
  double        *cm_mu;          /* mu for each partition's CM Gumbel   */
  double        *hmm_mu;         /* mu for each partition's HMM Gumbel  */
  float         *cm_minbitsc = NULL; /* minimum CM bit score to allow to pass for each partition */
  float         *cm_maxbitsc = NULL; /* maximum CM bit score to allow to pass for each partition */
  double         tmp_K;          /* used for recalc'ing Gumbel stats for DB size */
  int            was_hmmonly;    /* flag for if HMM only search was set */
  int            nalloc;         /* for cm_sc, hmm_sc, hmm_eval, num alloc'ed */
  int            chunksize;      /* allocation chunk size               */
  float         *scores;         /* CM and HMM score returned from worker*/
  void          *tmp;            /* temp void pointer for ESL_RALLOC() */
  int            clen;           /* consensus length of CM             */
  float          avg_hit_len;    /* crude estimate of average hit len  */
  int            Fidx;           /* index within hmm_eval              */
  float          S;              /* predicted survival fraction        */
  int            init_flag;      /* used for finding F                 */ 
  int            passed_flag = FALSE;
  float          cm_sc;
  float          orig_tau;
  float          hb_sc= 0.; 
  float          S_sc = 0.;
  float          Starget_sc = 0.;

#if defined(USE_MPI)  && defined(NOTDEFINED)
  int            have_work;      /* MPI: do we still have work to send out?*/
  int            nproc_working;  /* MPI: number procs currently doing work */
  int            wi;             /* MPI: worker index                      */
  ESL_SQ       **sqlist = NULL; /* MPI: queue of digitized seqs being worked on, 1..nproc-1 */
  int           *plist = NULL;   /* MPI: queue of partition indices of seqs being worked on, 1..nproc-1 */
  Parsetree_t  **trlist = NULL;  /* MPI: queue of traces of seqs being worked on, 1..nproc-1 */
  MPI_Status     mstatus;        /* useful info from MPI Gods         */
#endif


  /* TEMPORARY! */
  do_mpi = FALSE;
  printf("in FindCP9FilterThreshold fthr_mode: %d hmm_gum_mode: %d emit_mode: %d\n", fthr_mode, 
	 hmm_gum_mode, emit_mode);

  if(cm->search_opts & CM_SEARCH_HMMONLY) was_hmmonly = TRUE;
  else was_hmmonly = FALSE;
  cm->search_opts &= ~CM_SEARCH_HMMONLY;

  chunksize = 5 * N;
  nalloc    = chunksize;
  ESL_ALLOC(tr_sc,       sizeof(float) * nalloc);
  ESL_ALLOC(hmm_eval_p,  sizeof(float) * N);
  ESL_ALLOC(hmm_sc_p,    sizeof(float) * N);
  ESL_ALLOC(cm_sc_p,     sizeof(float) * N);
  ESL_ALLOC(cm_minbitsc, sizeof(float) * cmstats->np);
  ESL_ALLOC(cm_mu,       sizeof(double)* cmstats->np);
  ESL_ALLOC(hmm_mu,      sizeof(double)* cmstats->np);
  ESL_ALLOC(scores,      sizeof(float) * 2);
  
#if defined(USE_MPI) && defined(NOTDEFINED)
  if(do_mpi)
    {
      ESL_ALLOC(sqlist,      sizeof(ESL_SQ *)      * nproc);
      ESL_ALLOC(plist,       sizeof(int)           * nproc);
      ESL_ALLOC(trlist,      sizeof(Parsetree_t *) * nproc);
    }
#endif

  if(use_cm_cutoff) printf("CM E cutoff: %f\n", cm_ecutoff);
  else              printf("Not using CM cutoff\n");

  /* Configure the CM based on the emit mode COULD DIFFERENT FROM WORKERS! */
  ConfigForGumbelMode(cm, emit_mode);
  /* Copy the CM into cm_for_scoring, and reconfigure it if nec.,
   * We do this, so we change emission modes of the original CM */
  cm_for_scoring = DuplicateCM(cm); 
  /*if(emit_mode == CM_GC && (fthr_mode == CM_LC || fthr_mode == CM_LI))*/
  ConfigForGumbelMode(cm_for_scoring, fthr_mode);

  cm_CreateScanMatrixForCM(cm_for_scoring, TRUE, TRUE);

  /* Configure the HMM based on the hmm_gum_mode */
  if(hmm_gum_mode == CP9_L)
    {
      CPlan9SWConfig(cm_for_scoring->cp9, cm_for_scoring->pbegin, cm_for_scoring->pbegin);
      if(! (cm_for_scoring->flags & CMH_LOCAL_END))
	ConfigLocal(cm_for_scoring, cm_for_scoring->pbegin, cm_for_scoring->pend); 	/* need CM in local mode to calculate HMM EL probs, sloppy */
      CPlan9ELConfig(cm_for_scoring);
      if(! (cm_for_scoring->flags & CMH_LOCAL_END))
	ConfigGlobal(cm_for_scoring); 	/* return CM back to global mode, sloppy */
    }
  else /* hmm_gum_mode == CP9_G (it's in the contract) */
    CPlan9GlobalConfig(cm_for_scoring->cp9);
  CP9Logoddsify(cm_for_scoring->cp9);
  if(cm_for_scoring->config_opts & CM_CONFIG_ZEROINSERTS)
    CP9HackInsertScores(cm_for_scoring->cp9);

  /* Determine bit cutoff for each partition, calc'ed from cm_ecutoff */
  for (p = 0; p < cmstats->np; p++)
    {
      /* First determine mu based on db_size */
      tmp_K      = exp(cmstats->gumAA[hmm_gum_mode][p]->mu * cmstats->gumAA[hmm_gum_mode][p]->lambda) / 
	cmstats->gumAA[hmm_gum_mode][p]->L;
      hmm_mu[p]  = log(tmp_K * ((double) db_size)) / cmstats->gumAA[hmm_gum_mode][p]->lambda;
      tmp_K      = exp(cmstats->gumAA[fthr_mode][p]->mu * cmstats->gumAA[fthr_mode][p]->lambda) / 
	cmstats->gumAA[fthr_mode][p]->L;
      cm_mu[p]   = log(tmp_K  * ((double) db_size)) / cmstats->gumAA[fthr_mode][p]->lambda;
      /* Now determine bit score */
      cm_minbitsc[p] = cm_mu[p] - (log(cm_ecutoff) / cmstats->gumAA[fthr_mode][p]->lambda);
      if(use_cm_cutoff)
	printf("E: %f p: %d %d--%d bit score: %f\n", cm_ecutoff, p, 
	       cmstats->ps[p], cmstats->pe[p], cm_minbitsc[p]);
    }
  
  /* Do the work, emit parsetrees and collect the scores 
   * either in serial or MPI depending on do_mpi flag.
   */
  /*********************SERIAL BLOCK*************************************/
  int nleft = 0; /* number of seqs with scores < min CM score */
  int tr_np, tr_na, s1_np, s1_na, s2_np, s2_na, s3_np, s3_na;
  int do_slow = FALSE;
  char *name;
  if(Rpts_fp != NULL) do_slow = TRUE; /* we'll always find optimal CM parse to use as point for R 2D plot */

  tr_np = tr_na = s1_np = s1_na = s2_np = s2_na = s3_np = s3_na = 0;
  printf("06.11.07 Min np: %5d Smin: %12f Starget: %f Spad: %.3f do_Fstep: %d\n", N, Smin, Starget, Spad, do_Fstep);
  if(!(do_mpi))
    {
      
      orig_tau = cm_for_scoring->tau;

      /* Serial strategy: 
       * Emit sequences one at a time and search them with the CM and HMM,
       * keeping track of scores. If do_fastfil we don't have to search 
       * with the CM we just keep track of the parsetree score. 
       *
       * If do_fastfil && emit_mode is different than scoring mode we 
       * score with 'cm_for_scoring', instead of with 'cm'.
       */

      while(ip < N) /* while number seqs passed CM score threshold (ip) < N */
	{
	  ESL_ALLOC(name, sizeof(char) * 50);
	  sprintf(name, "seq%d", ip+1);
	  EmitParsetree(cm, r, name, TRUE, &tr, &sq, &L); /* TRUE: digitize the seq */
	  while(L == 0) { FreeParsetree(tr); esl_sq_Destroy(sq); EmitParsetree(cm, r, name, TRUE, &tr, &sq, &L); }
	  tr_na++;
	  passed_flag = FALSE;

	  p = cmstats->gc2p[(get_gc_comp(sq, 1, L))]; /* in get_gc_comp() should be i and j of best hit */
	  if(emit_mode == CM_GC && (fthr_mode == CM_LC || fthr_mode == CM_LI))
	    tr_sc[i] = ParsetreeScore_Global2Local(cm_for_scoring, tr, sq->dsq, FALSE);
	  else
	    tr_sc[i] = ParsetreeScore(cm_for_scoring, tr, sq->dsq, FALSE); 
	  /*
	  esl_sqio_Write(stdout, sq, eslSQFILE_FASTA, seq);
	  ParsetreeDump(stdout, tr, cm, sq);
	  printf("%d Parsetree Score: %f\n\n", i, tr_sc[i]);
	  */

	  /* If do_minmax, check if the parsetree score less than maximum allowed */
	  if(tr_sc[i] > cm_minbitsc[p] && !do_slow) /* we know we've passed */
	    {
	      tr_np++;
	      passed_flag = TRUE;
	      cm_sc_p[ip] = tr_sc[i];
	      //printf("TR P (P: %5d L: %5d)\n", ip, nleft);
	    }
	  else
	    {
	      /* we're not sure if our optimal score exceeds cm_minbitsc */
	      /* STAGE 1 */
	      
	      /* For speed first see if a strict (high tau) HMM banded search finds a 
	       * conditional optimal parse with score > min score */
	      s1_na++;
	      cm_for_scoring->search_opts |= CM_SEARCH_HBANDED;
	      //cm_for_scoring->tau = 0.1;
	      cm_for_scoring->tau = 0.01;
	      //cm_for_scoring->tau = 0.001;
	      
	      hb_sc = OldActuallySearchTarget(cm_for_scoring, sq->dsq, 1, L,
					      0.,    /* cutoff is 0 bits (actually we'll find highest
						     * negative score if it's < 0.0) */
					      0.,    /* CP9 cutoff is 0 bits */
					      NULL,  /* don't keep results */
					      FALSE, /* don't filter with a CP9 HMM */
					      FALSE, /* we're not calcing CM  stats */
					      FALSE, /* we're not calcing CP9 stats */
					      NULL,  /* filter fraction N/A */
					      FALSE);/* do NOT align the hits */
	      //if(!do_fastfil) printf("%4d %5d %d T: %10.4f BC: %10.4f ", ip, i, passed_flag, tr_sc[i], hb_sc);
	      if(hb_sc > cm_minbitsc[p] && !do_slow)
		{
		  s1_np++;
		  passed_flag = TRUE;
		  cm_sc_p[ip] = hb_sc;
		  //printf("S1 P (P: %5d L: %5d)\n", ip, nleft);
		}
	      else /* Stage 2, search with another, less strict (lower tau)  HMM banded parse */
		{
		  s2_na++;
		  cm_for_scoring->search_opts |= CM_SEARCH_HMMSCANBANDS;
		  cm_for_scoring->tau = 1e-15;
		  cm_sc = OldActuallySearchTarget(cm_for_scoring, sq->dsq, 1, L,
						 0.,    /* cutoff is 0 bits (actually we'll find highest
							 * negative score if it's < 0.0) */
						 0.,    /* CP9 cutoff is 0 bits */
						 NULL,  /* don't keep results */
						 FALSE, /* don't filter with a CP9 HMM */
						 FALSE, /* we're not calcing CM  stats */
						 FALSE, /* we're not calcing CP9 stats */
						 NULL,  /* filter fraction N/A */
						 FALSE);/* do NOT align the hits */
		  if(cm_sc > cm_minbitsc[p] && !do_slow)
		    {
		      s2_np++;
		      passed_flag = TRUE;
		      cm_sc_p[ip] = cm_sc;
		      //printf("S2 P (P: %5d L: %5d)\n", ip, nleft);
		    }
		  else /* search for the optimal parse */
		    {
		      s3_na++;
		      /* Stage 3 do QDB CYK */
		      cm_for_scoring->search_opts &= ~CM_SEARCH_HBANDED;
		      cm_for_scoring->search_opts &= ~CM_SEARCH_HMMSCANBANDS;
		      cm_sc = OldActuallySearchTarget(cm_for_scoring, sq->dsq, 1, L,
						     0.,    /* cutoff is 0 bits (actually we'll find highest
							     * negative score if it's < 0.0) */
						     0.,    /* CP9 cutoff is 0 bits */
						     NULL,  /* don't keep results */
						     FALSE, /* don't filter with a CP9 HMM */
						     FALSE, /* we're not calcing CM  stats */
						     FALSE, /* we're not calcing CP9 stats */
						     NULL,  /* filter fraction N/A */
						     FALSE);/* do NOT align the hits */
		      if(cm_sc > cm_minbitsc[p])
			{
			  s3_np++;
			  passed_flag = TRUE;
			  cm_sc_p[ip] = cm_sc;
			  //printf("S3 P (P: %5d L: %5d)\n", ip, nleft);
			}
		    }
		}
	    }
	  if(!passed_flag) 
	    {
	      nleft++;
	      //printf("LEFT (P: %5d L: %5d)\n", ip, nleft);
	    }
	  else if(passed_flag)
	    {
	      /* Scan seq with HMM */
	      /* DO NOT CALL OldActuallySearchTarget b/c that will run Forward then 
	       * Backward to get score of best hit, but we'll be detecting by a
	       * Forward scan (then running Backward only on hits above our threshold).
	       */
	      hmm_sc_p[ip] = CP9Forward(cm_for_scoring, sq->dsq, 1, L, cm_for_scoring->W, 0., 
					NULL,   /* don't return scores of hits */
					NULL,   /* don't return posns of hits */
					NULL,   /* don't keep track of hits */
					TRUE,   /* we're scanning */
					FALSE,  /* we're not ultimately aligning */
					FALSE,  /* we're not rescanning */
					TRUE,   /* be memory efficient */
					NULL);  /* don't want the DP matrix back */
	      hmm_eval_p[ip] = RJK_ExtremeValueE(hmm_sc_p[ip], hmm_mu[p], cmstats->gumAA[hmm_gum_mode][p]->lambda);
	      if(Rpts_fp != NULL)
		fprintf(Rpts_fp, "%.15f %.15f\n", hmm_sc_p[ip], cm_sc);
	      ip++; /* increase counter of seqs passing threshold */
	    }
	  esl_sq_Destroy(sq);
	  
	  /* Check if we need to reallocate */
	  i++;
	  if(i > imax) cm_Fail("ERROR number of attempts exceeded 500 times number of seqs.\n");
	  if (i == nalloc) 
	    {
	      nalloc += chunksize;
	      ESL_RALLOC(tr_sc,    tmp, nalloc * sizeof(float));
	    }
	  if(ip % N == 0)
	    {
	      ESL_RALLOC(hmm_eval_p, tmp, (ip + N) * sizeof(float));
	      ESL_RALLOC(hmm_sc_p,   tmp, (ip + N) * sizeof(float));
	      ESL_RALLOC(cm_sc_p,    tmp, (ip + N) * sizeof(float));
	    }
	}
    }
  N = ip; /* update N based on number of seqs sampled */
  printf("06.11.07 np: %5d nleft: %5d\n", ip, nleft);
  printf("06.11.07 tr_a: %5d tr_p: %5d\n06.11.07 s1_a: %5d s1_p: %5d\n06.11.07 s2_a: %5d s2_p: %5d\n06.11.07 s3_a: %5d s3_p: %5d\n", tr_na, tr_np, s1_na, s1_np, s2_na, s2_np, s3_na, s3_np);
  
  /*************************END OF SERIAL BLOCK****************************/
#if defined(USE_MPI)  && defined(NOTDEFINED)
  /*************************MPI BLOCK****************************/
  if(do_mpi)
    {
      /* MPI Strategy: 
       * Emit seqs one at a time from the CM, if the CM parse tree score is greater
       * than our max E-value (if do_minmax, otherwise there is no maximum),
       * send it to a worker. The worker then tries to quickly determine if
       * the sequence is within the acceptable E-value range by doing HMM
       * banded searches. If the sequence is within the E-value range,
       * it is searched with an HMM. Both the optimal CM parse scores and HMM
       * scores are passed back to the master, whose keeping track of how
       * many seqs have been sampled within the E-value range.
       *
       * If do_fastfil, the worker skips the CM search and returns 0. bits
       * as CM score, which master replaces with parsetree score. (This is 
       * an old strategy I'm probably about to deprecate (06.07.07))
       *
       * Sean's design pattern for data parallelization in a master/worker model:
       * three phases: 
       *  1. load workers;
       *  2. recv result/send work loop;
       *  3. collect remaining results
       * but implemented in a single while loop to avoid redundancy.
       */
      have_work     = TRUE;
      nproc_working = 0;
      wi            = 1;
      i             = 0;
      while (have_work || nproc_working)
	{
	  /* Get next work unit. */
	  if(ip < N) /* if number seqs passed CM score threshold (ip) < N */
	    {
	      tr_passed_flag = FALSE;
	      while(!tr_passed_flag)
		{
		  sprintf(name, "seq%d", ip+1);
		  EmitParsetree(cm, r, name, TRUE, &tr, &sq, &L); /* TRUE: digitize the seq */
		  while(L == 0) { FreeParsetree(tr); esl_sq_Destroy(sq); EmitParsetree(cm, r, name, TRUE, &tr, &sq, &L); }

		  p = cmstats->gc2p[(get_gc_comp(sq, 1, L))]; /* in get_gc_comp() should be i and j of best hit */
		  free(seq);
		  /*ParsetreeDump(stdout, tr, cm, sq);
		    printf("%d Parsetree Score: %f\n\n", (nattempts), ParsetreeScore(cm, tr, dsq, FALSE)); */
		  if(emit_mode == CM_GC && (fthr_mode == CM_LC || fthr_mode == CM_LI))
		    tr_sc[i] = ParsetreeScore_Global2Local(cm_for_scoring, trlist[wi], dsqlist[wi], FALSE);
		  else
		    tr_sc[i] = ParsetreeScore(cm_for_scoring, tr, dsq, FALSE); 
		  FreeParsetree(tr);
		  /* If do_minmax, check if the parsetree score less than maximum allowed */
		  if((!do_minmax || (do_minmax && (tr_sc[i] <= cm_maxbitsc[p]))))
		    tr_passed_flag = TRUE;
		}		    
	      if(!do_fastfil)
		FreeParsetree(tr);
	    }
	  else have_work = FALSE;
	  /* If we have work but no free workers, or we have no work but workers
	   * Are still working, then wait for a result to return from any worker.
	   */
	  if ( (have_work && nproc_working == nproc-1) || (! have_work && nproc_working > 0))
	    {
	      MPI_Recv(scores,  2, MPI_FLOAT, MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &mstatus);
	      cm_sc    = scores[0];
	      hmm_sc   = scores[1];
	      wi = mstatus.MPI_SOURCE;

	      if(do_fastfil)
		{
		  if(emit_mode == CM_GC && (fthr_mode == CM_LC || fthr_mode == CM_LI))
		    cm_sc = ParsetreeScore_Global2Local(cm_for_scoring, trlist[wi], dsqlist[wi], FALSE);
		  else
		    cm_sc = ParsetreeScore(cm_for_scoring, trlist[wi], dsqlist[wi], FALSE); 
		  FreeParsetree(trlist[wi]);
		  trlist[wi] = NULL;
		}
	      if(ip < N && 
		 ( do_minmax && (cm_sc >= cm_minbitsc[plist[wi]] && cm_sc <= cm_maxbitsc[plist[wi]])) ||
		 (!do_minmax && (cm_sc >= cm_minbitsc[plist[wi]])))
		{
		  cm_sc_p[ip]    = cm_sc;
		  hmm_sc_p[ip]   = hmm_sc;
		  hmm_eval_p[ip] = RJK_ExtremeValueE(hmm_sc, hmm_mu[plist[wi]], cmstats->gumAA[hmm_gum_mode][plist[wi]]->lambda);
		  ip++; /* increase counter of seqs passing threshold */
		}
	      nproc_working--;
	      free(dsqlist[wi]);
	      dsqlist[wi] = NULL;
	      
	      /* Check if we need to reallocate */
	      i++;
	      if(i > imax) cm_Fail("ERROR number of attempts exceeded 500 times number of seqs.\n");
	      if (i == nalloc) 
		{
		  nalloc += chunksize;
		  ESL_RALLOC(tr_sc,    tmp, nalloc * sizeof(float));
		}
	    }
	  /* If we have work, assign it to a free worker;
	   * else, terminate the free worker.
	   */
	  if (have_work) 
	    {
	      dsq_maxsc_MPISend(dsq, L, cm_maxbitsc[p], wi);
	      dsqlist[wi] = dsq;
	      plist[wi]   = p;
	      if(do_fastfil)
		trlist[wi] = tr;
	      wi++;
	      nproc_working++;
	    }
	  else 
	    dsq_maxsc_MPISend(NULL, -1, -1, wi);	
	}
    }
  /*********************END OF MPI BLOCK*****************************/
#endif /* if defined(USE_MPI) */
  /* Sort the HMM E-values with quicksort */
  esl_vec_FSortIncreasing(hmm_eval_p, N);
  esl_vec_FSortDecreasing(hmm_sc_p, N); /* TEMPORARY FOR PRINTING */

  /* Determine E-value to return based on predicted filter survival fraction */
  clen = (2*CMCountStatetype(cm, MATP_MP) + 
	  CMCountStatetype(cm, MATL_ML) + 
	  CMCountStatetype(cm, MATR_MR));
  avg_hit_len = clen; /* currently, we don't correct for local hits probably being shorter */

  /* Strategy 1, enforce minimum F, but look for highest that gives survival fraction of at least
   * Starget */
  init_flag  = TRUE;
  S          = IMPOSSIBLE;
  Emin    = ((double) db_size * Smin)    / ((2. * cm_for_scoring->W) - avg_hit_len);
  Etarget = ((double) db_size * Starget) / ((2. * cm_for_scoring->W) - avg_hit_len);
  printf("Smin: %12f\nStarget: %f\n", Smin, Starget);
  printf("Emin: %12f\nEtarget: %f\n", Emin, Etarget);

  Fidx  = (int) (Fmin * (float) N) - 1; /* off by one, ex: 95% cutoff in 100 len array is index 94 */
  E     = hmm_eval_p[Fidx];
  F     = Fmin;
  /* Are we case 1 or case 2? 
   * Case 1: our E is greater than our Etarget, so we cannot satisfy Starget AND capture F fraction of
   *         the true CM hits, it's more impt we capture F fraction, so return S > Starget.
   * Case 2: our E is less than our Etarget so we can satisfy both criteria, we have to choose an S
   *         now. One option is If (do_Fstep) increase F toward 1.0 while E < Etarget. Then add
   *         Spad fraction of (bitscore(S) - bitscore(max(Starget, Smin))) to bitscore(S), and calculate
   *         the new S. If Spad == 0., speed trumps filter safety, if S == 1.0, we want the filter
   *         as safe as possible, we're returning S = Starget. 
   */
  if(E > Etarget) 
    {
      F = ((float) (Fidx + 1.)) / (float) N;
      S = (E * ((2. * cm_for_scoring->W) - avg_hit_len)) / ((double) db_size); 
      if(E < Emin)  /* no need for E < Emin */
	  printf("Case 1A rare case: init Emin %12f > E %f > Etarget %f F: %f S: %.12f Spad: %.3f\n", Emin, E, Etarget, F, S, Spad);
      else
	  printf("Case 1B bad  case: init E %f > Etarget %f && E > Emin %12f F: %f S: %.12f Spad: %.3f\n", E, Etarget, Emin, F, S, Spad);
      if(E < Emin) E = Emin; /* No reason to have an E below Emin */
    }
  else
    {
      /* If(do_Fstep): increase F until we're about to go over Etarget (which gives our target 
	 survival fraction Starget). */
      if(do_Fstep)
	while(((Fidx+1) < N) && hmm_eval_p[(Fidx+1)] < Etarget)
	  Fidx++;
      F = ((float) (Fidx + 1.)) / (float) N;

      /* Subtract Spad * (score(S) - score(max(Starget, Smin))) from score(S) and recalculate E&S */
      /* Use partition that covers GC = 50 */
      p = cm->stats->gc2p[50];
      E = hmm_eval_p[Fidx];
      printf("Before Spad E: %f\n", E);
      S_sc =    (cm->stats->gumAA[hmm_gum_mode][p]->mu - 
		 (log(E) / cm->stats->gumAA[hmm_gum_mode][p]->lambda)); 
      Starget_sc = (cm->stats->gumAA[hmm_gum_mode][p]->mu - 
		    (log(Etarget) / cm->stats->gumAA[hmm_gum_mode][p]->lambda));

      printf("S_sc 0: %.3f - %.3f * %.3f = ", S_sc, Spad, S_sc - Starget_sc);
      S_sc -= Spad * (S_sc - Starget_sc); /* Spad may be 0. */

      printf(" S1: %.3f\n", S_sc);
      /* now recalculate what E and S should be based on S_sc */
      E = RJK_ExtremeValueE(S_sc, cm->stats->gumAA[hmm_gum_mode][p]->mu, 
			    cmstats->gumAA[hmm_gum_mode][p]->lambda);
      S = (E * ((2. * cm_for_scoring->W) - avg_hit_len)) / ((double) db_size); 

      if(fabs(E - Emin) < 0.01 || E < Emin)  /* no need for E < Emin */
	printf("Case 2A: best case Emin %12f > E %f < Etarget %f F: %f S: %.12f Spad: %.3f\n", Emin, E, Etarget, F, S, Spad);
       else
	printf("Case 2B: good case Emin %12f < E %f < Etarget %f F: %f S: %.12f\n Spad: %.3f", Emin, E, Etarget, F, S, Spad);
      if(E < Emin) E = Emin; /* No reason to have an E below Emin */
    }
  /* Print cutoff info to Rpts file for 2D plot if nec */
  if(Rpts_fp != NULL)
    {
      fprintf(Rpts_fp, "TSC %.15f\n", (cm->stats->gumAA[hmm_gum_mode][p]->mu - 
				       (log(Etarget) / cm->stats->gumAA[hmm_gum_mode][p]->lambda)));
      fprintf(Rpts_fp, "MSC %.15f\n", (cm->stats->gumAA[hmm_gum_mode][p]->mu - 
				       (log(Emin) / cm->stats->gumAA[hmm_gum_mode][p]->lambda)));
      fprintf(Rpts_fp, "FSC %.15f\n", (cm->stats->gumAA[hmm_gum_mode][p]->mu - 
					(log(E) / cm->stats->gumAA[hmm_gum_mode][p]->lambda)));
      fprintf(Rpts_fp, "FMINSC %.15f\n", (cm->stats->gumAA[hmm_gum_mode][p]->mu - 
					(log(hmm_eval_p[(int) (Fmin * (float) N)-1]) / cm->stats->gumAA[hmm_gum_mode][p]->lambda)));
      fprintf(Rpts_fp, "F   %.15f\n", F);
      fprintf(Rpts_fp, "CMGUM %.15f %15f\n", cm->stats->gumAA[emit_mode][p]->lambda, cm->stats->gumAA[emit_mode][p]->mu);
      fprintf(Rpts_fp, "HMMGUM %.15f %15f\n", cm->stats->gumAA[hmm_gum_mode][p]->lambda, cm->stats->gumAA[hmm_gum_mode][p]->mu);
      fprintf(Rpts_fp, "STARGET %.15f\n", Starget);
      fprintf(Rpts_fp, "SMIN %.15f\n", Smin);
      fprintf(Rpts_fp, "SPAD %.15f\n", Spad);
      fprintf(Rpts_fp, "FMIN %.15f\n", Fmin);
      fprintf(Rpts_fp, "FSTEP %d\n", do_Fstep);
      fprintf(Rpts_fp, "ECUTOFF %.15f\n", cm_ecutoff);
      for (p = 0; p < cmstats->np; p++)
	fprintf(Rpts_fp, "SCCUTOFF %d %.15f\n", p, cm_minbitsc[p]);
      fclose(Rpts_fp);
    }	  

  /* Make sure our E is less than the DB size and greater than Emin */
  if(E > ((float) db_size)) /* E-val > db_size is useless */
    {
      printf("Case 3 : worst case E (%f) > db_size (%f)\n", E, (double) db_size);
      E = (float) db_size;
    }  
  
  /* Informative, temporary print statements */
  for (i = ((int) (Fmin  * (float) N) -1); i < N; i++)
    printf("%d i: %4d hmm sc: %10.4f hmm E: %10.4f\n", hmm_gum_mode, i, hmm_sc_p[i], hmm_eval_p[i]);
  printf("\nSummary: %d %d %d %d %f %f\n", fthr_mode, hmm_gum_mode, do_fastfil, emit_mode,
	 (hmm_sc_p[Fidx]), (hmm_eval_p[Fidx]));
  printf("05.21.07 %d %d %f %f\n", fthr_mode, hmm_gum_mode, hmm_sc_p[Fidx], E);

  /* Reset CM_SEARCH_HMMONLY search option as it was when function was entered */
  if(was_hmmonly) cm->search_opts |= CM_SEARCH_HMMONLY;
  else cm->search_opts &= ~CM_SEARCH_HMMONLY;

  /* Clean up and exit */
  free(tr_sc);
  free(hmm_eval_p);
  free(hmm_sc_p);
  free(cm_sc_p);
  free(hmm_mu);
  free(cm_mu);
  free(cm_minbitsc);
  free(cm_maxbitsc);
  free(scores);
  cm_for_scoring->tau = orig_tau;
  FreeCM(cm_for_scoring);
  /* Return threshold */
  *ret_F = F;
  printf("F: %f\n", *ret_F);
  return E;

 ERROR:
  cm_Fail("Reached ERROR in FindCP9FilterThreshold()\n");
  return 0.;
}
