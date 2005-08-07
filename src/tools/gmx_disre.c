/*
 * $Id$
 * 
 *                This source code is part of
 * 
 *                 G   R   O   M   A   C   S
 * 
 *          GROningen MAchine for Chemical Simulations
 * 
 *                        VERSION 3.2.0
 * Written by David van der Spoel, Erik Lindahl, Berk Hess, and others.
 * Copyright (c) 1991-2000, University of Groningen, The Netherlands.
 * Copyright (c) 2001-2004, The GROMACS development team,
 * check out http://www.gromacs.org for more information.

 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * If you want to redistribute modifications, please consider that
 * scientific software is very special. Version control is crucial -
 * bugs must be traceable. We will be happy to consider code for
 * inclusion in the official distribution, but derived work must not
 * be called official GROMACS. Details are found in the README & COPYING
 * files - if they are missing, get the official version at www.gromacs.org.
 * 
 * To help us fund GROMACS development, we humbly ask that you cite
 * the papers on the package - you can find them in the top README file.
 * 
 * For more info, check our website at http://www.gromacs.org
 * 
 * And Hey:
 * Green Red Orange Magenta Azure Cyan Skyblue
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <math.h>
#include <string.h>

#include "typedefs.h"
#include "macros.h"
#include "copyrite.h"
#include "mshift.h"
#include "xvgr.h"
#include "vec.h"
#include "do_fit.h"
#include "confio.h"
#include "smalloc.h"
#include "nrnb.h"
#include "disre.h"
#include "statutil.h"
#include "force.h"
#include "gstat.h"
#include "main.h"
#include "pdbio.h"
#include "index.h"
#include "mdatoms.h"
#include "nsb.h"
#include "tpxio.h"
#include "mdrun.h"
#include "names.h"

typedef struct {
  int n;
  real v;
} t_toppop;

t_toppop *top=NULL;
int      ntop=0;

typedef struct {
  int  nv,nframes;
  real sumv,averv,maxv;
  real *aver1,*aver2,*aver_3,*aver_6;
} t_dr_result;

static void init5(int n)
{
  ntop=n;
  snew(top,ntop);
}

static void reset5(void)
{
  int i;

  for(i=0; (i<ntop); i++) {
    top[i].n=-1;
    top[i].v= 0;
  }
}

int tpcomp(const void *a,const void *b)
{
  t_toppop *tpa;
  t_toppop *tpb;

  tpa=(t_toppop *)a;
  tpb=(t_toppop *)b;

  return  (1e7*(tpb->v-tpa->v));
}

static void add5(int ndr,real viol)
{
  int i,mini;
  
  mini=0;
  for(i=1; (i<ntop); i++)
    if (top[i].v < top[mini].v) 
      mini=i;
  if (viol > top[mini].v) {
    top[mini].v=viol;
    top[mini].n=ndr;
  }
}

static void print5(FILE *fp)
{
  int i;

  qsort(top,ntop,sizeof(top[0]),tpcomp);
  fprintf(fp,"Index:");
  for(i=0; (i<ntop); i++)
    fprintf(fp," %6d",top[i].n);
  fprintf(fp,"\nViol: ");
  for(i=0; (i<ntop); i++)
    fprintf(fp," %6.3f",top[i].v);
  fprintf(fp,"\n");
}

static void check_viol(FILE *log,t_commrec *mcr,
		       t_ilist *disres,t_iparams forceparams[],
		       t_functype functype[],rvec x[],rvec f[],
		       t_forcerec *fr,t_pbc *pbc,t_graph *g,t_dr_result dr[],
		       int clust_id,int isize,atom_id index[],real vvindex[],
		       t_fcdata *fcd)
{
  t_iatom *forceatoms;
  int     i,j,nat,n,type,nviol,ndr,label;
  real    ener,rt,mviol,tviol,viol,lam,dvdl,drt;
  static  bool bFirst=TRUE;
  
  lam  =0;
  dvdl =0;
  tviol=0;
  nviol=0;
  mviol=0;
  ndr=0;
  if (ntop)
    reset5();
  forceatoms=disres->iatoms;
  for(j=0; (j<isize); j++) {
    vvindex[j]=0;
  }
  nat = interaction_function[F_DISRES].nratoms+1; 
  for(i=0; (i<disres->nr); ) {
    type  = forceatoms[i];
    n     = 0;
    label = forceparams[type].disres.label;
    if (debug) 
      fprintf(debug,"DISRE: ndr = %d, label = %d  i=%d, n =%d\n",
	      ndr,label,i,n);
    if (ndr != label) 
      gmx_fatal(FARGS,"tpr inconsistency. ndr = %d, label = %d\n",ndr,label);
    do {
      n += nat;
    } while (((i+n) < disres->nr) && 
	     (forceparams[forceatoms[i+n]].disres.label == label));
    
    calc_disres_R_6(mcr,n,&forceatoms[i],forceparams,
		    (const rvec*)x,pbc,fcd);

    rt = pow(fcd->disres.Rt_6[0],-1.0/6.0);
    dr[clust_id].aver1[ndr]  += rt;
    dr[clust_id].aver2[ndr]  += sqr(rt);
    drt = pow(rt,-3.0);
    if ((rt <=0) || (drt <= 0))
      gmx_fatal(FARGS,"ndr = %d, rt = %8.3f, drt = %8.3f",ndr,rt,drt);
    dr[clust_id].aver_3[ndr] += drt;
    dr[clust_id].aver_6[ndr] += fcd->disres.Rt_6[0];
    
    ener=interaction_function[F_DISRES].ifunc(n,&forceatoms[i],forceparams,
					      (const rvec*)x,f,fr->fshift,
					      pbc,g,lam,&dvdl,NULL,fcd);
    viol = fcd->disres.sumviol;
    
    
    if (viol > 0) {
      nviol++;
      if (ntop)
	add5(forceparams[type].disres.label,viol);
      if (viol > mviol) 
	mviol = viol;
      tviol += viol;
      for(j=0; (j<isize); j++) {
	if (index[j] == forceparams[type].disres.label)
	  vvindex[j]=pow(fcd->disres.Rt_6[0],-1.0/6.0);
	}
    }
    ndr ++;
    i   += n;
  }
  dr[clust_id].nv   = nviol;
  dr[clust_id].maxv = mviol;
  dr[clust_id].sumv = tviol;
  dr[clust_id].averv= tviol/ndr;
  dr[clust_id].nframes++;
  
  if (bFirst) {
    fprintf(stderr,"\nThere are %d restraints and %d pairs\n",
	    ndr,disres->nr/nat);
    bFirst = FALSE;
  }
  if (ntop)
    print5(log);
}

typedef struct {
  int  index;
  bool bCore;
  real up1,r,rT3,rT6,viol,violT3,violT6;
} t_dr_stats;

static int drs_comp(const void *a,const void *b)
{
  t_dr_stats *da,*db;
  
  da = (t_dr_stats *)a;
  db = (t_dr_stats *)b;
  
  if (da->viol > db->viol)
    return -1;
  else if (da->viol < db->viol)
    return 1;
  else
    return 0;
}

static void dump_dump(FILE *log,int ndr,t_dr_stats drs[])
{
  static char *core[] = { "All restraints", "Core restraints" };
  static char *tp[]   = { "linear", "third power", "sixth power" };
  real viol_tot,viol_max,viol;
  bool bCore;
  int  nviol,nrestr;
  int  i,kkk;

  for(bCore = FALSE; (bCore <= TRUE); bCore++) {
    for(kkk=0; (kkk<3); kkk++) {
      viol_tot  = 0;
      viol_max  = 0;
      nviol     = 0;
      nrestr    = 0;
      for(i=0; (i<ndr); i++) {
	if (!bCore || (bCore && drs[i].bCore)) {
	  switch (kkk) {
	  case 0:
	    viol = drs[i].viol;
	    break;
	  case 1: 
	    viol = drs[i].violT3;
	    break;
	  case 2:
	    viol = drs[i].violT6;
	    break;
	  default:
	    gmx_incons("Dumping violations");
	  }
	  viol_max     = max(viol_max,viol);
	  if (viol > 0)
	    nviol++;
	  viol_tot  += viol;
	  nrestr++;
	}
      }
      if ((nrestr > 0) || (bCore && (nrestr < ndr))) {
	fprintf(log,"\n");
	fprintf(log,"+++++++ %s ++++++++\n",core[bCore]);
	fprintf(log,"+++++++ Using %s averaging: ++++++++\n",tp[kkk]);
	fprintf(log,"Sum of violations: %8.3f nm\n",viol_tot);
	fprintf(log,"Average violation: %8.3f nm\n",viol_tot/nrestr);
	fprintf(log,"Largest violation: %8.3f nm\n",viol_max);
	fprintf(log,"Number of violated restraints: %d/%d\n",nviol,nrestr);
      }
    }
  }
}

static void dump_viol(FILE *log,int ndr,t_dr_stats *drs,bool bLinear)
{
  int i;
  
  fprintf(log,"Restr. Core     Up1     <r>   <rT3>   <rT6>  <viol><violT3><violT6>\n");
  for(i=0; (i<ndr); i++) {
    if (bLinear  && (drs[i].viol == 0))
      break;
    fprintf(log,"%6d%5s%8.3f%8.3f%8.3f%8.3f%8.3f%8.3f%8.3f\n",
	    drs[i].index,yesno_names[drs[i].bCore],
	    drs[i].up1,drs[i].r,drs[i].rT3,drs[i].rT6,
	    drs[i].viol,drs[i].violT3,drs[i].violT6);
  }
}

static bool is_core(int i,int isize,atom_id index[])
{
  int kk;
  bool bIC = FALSE;
  
  for(kk=0; !bIC && (kk<isize); kk++)
    bIC = (index[kk] == i);
    
  return bIC;
}	      

static void dump_stats(FILE *log,int nsteps,int ndr,t_ilist *disres,
		       t_iparams ip[],t_dr_result *dr,
		       int isize,atom_id index[],t_atoms *atoms)
{
  int  i,j,nra;
  t_dr_stats *drs;

  fprintf(log,"\n");
  fprintf(log,"++++++++++++++ STATISTICS ++++++++++++++++++++++++\n");  
  snew(drs,ndr);
  j         = 0;
  nra       = interaction_function[F_DISRES].nratoms+1;
  for(i=0; (i<ndr); i++) {
    /* Search for the appropriate restraint */
    for( ; (j<disres->nr); j+=nra) {
      if (ip[disres->iatoms[j]].disres.label == i)
	break;
    }
    drs[i].index  = i;
    drs[i].bCore  = is_core(i,isize,index);
    drs[i].up1    = ip[disres->iatoms[j]].disres.up1;
    drs[i].r      = dr->aver1[i]/nsteps;
    drs[i].rT3    = pow(dr->aver_3[i]/nsteps,-1.0/3.0);
    drs[i].rT6    = pow(dr->aver_6[i]/nsteps,-1.0/6.0);
    drs[i].viol   = max(0,drs[i].r-drs[i].up1);
    drs[i].violT3 = max(0,drs[i].rT3-drs[i].up1);
    drs[i].violT6 = max(0,drs[i].rT6-drs[i].up1);
    if (atoms) {
      int j1 = disres->iatoms[j+1];
      int j2 = disres->iatoms[j+2];
      atoms->pdbinfo[j1].bfac += drs[i].violT3*5;
      atoms->pdbinfo[j2].bfac += drs[i].violT3*5;
    }
  }
  dump_viol(log,ndr,drs,FALSE);
  
  fprintf(log,"+++ Sorted by linear averaged violations: +++\n");
  qsort(drs,ndr,sizeof(drs[0]),drs_comp);
  dump_viol(log,ndr,drs,TRUE);
	      
  dump_dump(log,ndr,drs);
  
  sfree(drs);
}

static void dump_clust_stats(FILE *fp,int ndr,t_ilist *disres,
			     t_iparams ip[],t_block *clust,t_dr_result dr[],
			     char *clust_name[],int isize,atom_id index[])
{
  int    i,j,k,nra,mmm=0;
  double sumV,maxV,sumVT3,sumVT6,maxVT3,maxVT6;
  t_dr_stats *drs;

  fprintf(fp,"\n");
  fprintf(fp,"++++++++++++++ STATISTICS ++++++++++++++++++++++\n");  
  fprintf(fp,"Cluster  NFrames    SumV      MaxV     SumVT     MaxVT\n");

  snew(drs,ndr);
  
  for(k=0; (k<clust->nr); k++) {
    if (dr[k].nframes == 0)
      continue;
    if (dr[k].nframes != (clust->index[k+1]-clust->index[k])) 
      gmx_fatal(FARGS,"Inconsistency in cluster %s.\n"
		"Found %d frames in trajectory rather than the expected %d\n",
		clust_name[k],dr[k].nframes,
		clust->index[k+1]-clust->index[k]);
    if (!clust_name[k])
      gmx_fatal(FARGS,"Inconsistency with cluster %d. Invalid name",k);
    j         = 0;
    nra       = interaction_function[F_DISRES].nratoms+1;
    sumV = sumVT3 = sumVT6 = maxV = maxVT3 = maxVT6 = 0;
    for(i=0; (i<ndr); i++) {
      /* Search for the appropriate restraint */
      for( ; (j<disres->nr); j+=nra) {
	if (ip[disres->iatoms[j]].disres.label == i)
	  break;
      }
      drs[i].index  = i;
      drs[i].bCore  = is_core(i,isize,index);
      drs[i].up1    = ip[disres->iatoms[j]].disres.up1;
      drs[i].r      = dr[k].aver1[i]/dr[k].nframes;
      if ((dr[k].aver_3[i] <= 0) || (dr[k].aver_3[i] != dr[k].aver_3[i]))
	gmx_fatal(FARGS,"dr[%d].aver_3[%d] = %f",k,i,dr[k].aver_3[i]);
      drs[i].rT3    = pow(dr[k].aver_3[i]/dr[k].nframes,-1.0/3.0);
      drs[i].rT6    = pow(dr[k].aver_3[i]/dr[k].nframes,-1.0/3.0);
      drs[i].viol   = max(0,drs[i].r-drs[i].up1);
      drs[i].violT3 = max(0,drs[i].rT3-drs[i].up1);
      drs[i].violT6 = max(0,drs[i].rT6-drs[i].up1);
      sumV   += drs[i].viol;
      sumVT3 += drs[i].violT3;
      sumVT6 += drs[i].violT6;
      maxV    = max(maxV,drs[i].viol);
      maxVT3  = max(maxVT3,drs[i].violT3);
      maxVT6  = max(maxVT6,drs[i].violT6);
    }
    if (strcmp(clust_name[k],"1000") == 0) {
      mmm ++;
    }
    fprintf(fp,"%-10s%6d%8.3f  %8.3f  %8.3f  %8.3f  %8.3f  %8.3f\n",
	    clust_name[k],
	    dr[k].nframes,sumV,maxV,sumVT3,maxVT3,sumVT6,maxVT6);
  
  }
  fflush(fp);
  sfree(drs);
}

static void init_dr_res(t_dr_result *dr,int ndr)
{
  snew(dr->aver1,ndr+1);
  snew(dr->aver2,ndr+1);
  snew(dr->aver_3,ndr+1);
  snew(dr->aver_6,ndr+1);
  dr->nv      = 0;
  dr->nframes = 0;
  dr->sumv    = 0;
  dr->maxv    = 0;
  dr->averv   = 0;
}

int gmx_disre(int argc,char *argv[])
{
  static char *desc[] = {
    "g_disre computes violations of distance restraints.",
    "If necessary all protons can be added to a protein molecule ",
    "using the protonate program.[PAR]",
    "The program allways",
    "computes the instantaneous violations rather than time-averaged,",
    "because this analysis is done from a trajectory file afterwards",
    "it does not make sense to use time averaging. However,",
    "the time averaged values per restraint are given in the log file.[PAR]",
    "An index file may be used to select specific restraints for",
    "printing.[PAR]",
    "When the optional[TT]-q[tt] flag is given a pdb file coloured by the",
    "amount of average violations.[PAR]",
    "When the [TT]-c[tt] option is given, an index file will be read",
    "containing the frames in your trajectory corresponding to the clusters",
    "(defined in another manner) that you want to analyze. For these clusters",
    "the program will compute average violations using the thisd power",
    "averaging algorithm and print them in the log file."
  };
  static int  ntop      = 0;
  t_pargs pa[] = {
    { "-ntop", FALSE, etINT,  {&ntop},
      "Number of large violations that are stored in the log file every step" }
  };
  
  FILE        *out,*aver,*numv,*maxxv,*xvg=NULL;
  t_tpxheader header;
  t_inputrec  ir;
  t_topology  top;
  rvec        *xtop;
  t_atoms     *atoms=NULL;
  t_forcerec  *fr;
  t_fcdata    fcd;
  t_nrnb      nrnb;
  t_nsborder  *nsb;
  t_commrec   *cr;
  t_graph     *g;
  int         status,ntopatoms,natoms,i,j,step,kkk,m;
  real        t,lambda;
  rvec        *x,*f,*xav=NULL;
  matrix      box;
  bool        bPDB;
  int         isize;
  atom_id     *index=NULL,*ind_fit=NULL;
  char        *grpname;
  t_cluster_ndx *clust=NULL;
  t_dr_result dr,*dr_clust;
  char        **leg;
  real        *vvindex=NULL,*w_rls=NULL;
  t_mdatoms   *mdatoms;
  t_pbc       pbc;
  
  t_filenm fnm[] = {
    { efTPX, NULL, NULL, ffREAD },
    { efTRX, "-f", NULL, ffREAD },
    { efXVG, "-ds", "drsum",  ffWRITE },
    { efXVG, "-da", "draver", ffWRITE },
    { efXVG, "-dn", "drnum",  ffWRITE },
    { efXVG, "-dm", "drmax",  ffWRITE },
    { efXVG, "-dr", "restr",  ffWRITE },
    { efLOG, "-l",  "disres", ffWRITE },
    { efNDX, NULL,  "viol",   ffOPTRD },
    { efPDB, "-q",  "viol",   ffOPTWR },
    { efNDX, "-c",  "clust",  ffOPTRD }
  };
#define NFILE asize(fnm)

  cr  = init_par(&argc,&argv);
  CopyRight(stderr,argv[0]);
  parse_common_args(&argc,argv,PCA_CAN_TIME | PCA_CAN_VIEW | PCA_BE_NICE,
		    NFILE,fnm,asize(pa),pa,asize(desc),desc,0,NULL);

  open_log(ftp2fn(efLOG,NFILE,fnm),cr);
  
  if (ntop)
    init5(ntop);
  
  read_tpxheader(ftp2fn(efTPX,NFILE,fnm),&header,FALSE,NULL,NULL);
  snew(xtop,header.natoms);
  read_tpx(ftp2fn(efTPX,NFILE,fnm),&step,&t,&lambda,&ir,
	   box,&ntopatoms,xtop,NULL,NULL,&top);
  bPDB = opt2bSet("-q",NFILE,fnm);
  if (bPDB) {
    snew(xav,ntopatoms);
    snew(ind_fit,ntopatoms);
    snew(w_rls,ntopatoms);
    for(kkk=0; (kkk<ntopatoms); kkk++) {
      w_rls[kkk] = 1;
      ind_fit[kkk] = kkk;
    }   
    if (top.atoms.pdbinfo == NULL)
      snew(top.atoms.pdbinfo,ntopatoms);
  } 
  check_nnodes_top(ftp2fn(efTPX,NFILE,fnm),&top,1);

  if (ir.ePBC == epbcXYZ) {
    g = mk_graph(&top.idef,top.atoms.nr,FALSE,FALSE);
  } else {
    g = NULL;
  }
  
  if (ftp2bSet(efNDX,NFILE,fnm)) {
    rd_index(ftp2fn(efNDX,NFILE,fnm),1,&isize,&index,&grpname);
    xvg=xvgropen(opt2fn("-dr",NFILE,fnm),"Inidividual Restraints","Time (ps)",
		 "nm");
    snew(vvindex,isize);
    snew(leg,isize);
    for(i=0; (i<isize); i++) {
      index[i]++;
      snew(leg[i],12);
      sprintf(leg[i],"index %d",index[i]);
    }
    xvgr_legend(xvg,isize,leg);
  }
  else 
    isize=0;

  ir.dr_tau=0.0;
  init_disres(stdlog,top.idef.il[F_DISRES].nr,top.idef.il[F_DISRES].iatoms,
	      top.idef.iparams,&ir,NULL,&fcd);

  natoms=read_first_x(&status,ftp2fn(efTRX,NFILE,fnm),&t,&x,box);
  snew(f,5*natoms);
  
  init_dr_res(&dr,fcd.disres.nr);
  if (opt2bSet("-c",NFILE,fnm)) {
    clust = cluster_index(opt2fn("-c",NFILE,fnm));
    snew(dr_clust,clust->clust->nr+1);
    for(i=0; (i<=clust->clust->nr); i++)
      init_dr_res(&dr_clust[i],fcd.disres.nr);
  }
  else {	
    out =xvgropen(opt2fn("-ds",NFILE,fnm),
		  "Sum of Violations","Time (ps)","nm");
    aver=xvgropen(opt2fn("-da",NFILE,fnm),
		  "Average Violation","Time (ps)","nm");
    numv=xvgropen(opt2fn("-dn",NFILE,fnm),
		  "# Violations","Time (ps)","#");
    maxxv=xvgropen(opt2fn("-dm",NFILE,fnm),
		   "Largest Violation","Time (ps)","nm");
  }

  snew(nsb,1);
  snew(atoms,1);
  atoms->nr=top.atoms.nr;
  atoms->nres=top.atoms.nres;
  snew(atoms->atomname,atoms->nr);
  snew(atoms->resname,atoms->nres);
  snew(atoms->atom,atoms->nr);
  memcpy(atoms->atom,top.atoms.atom,atoms->nr*sizeof(atoms->atom[0]));
  memcpy(atoms->atomname,top.atoms.atomname,
	 atoms->nr*sizeof(atoms->atomname[0]));
  memcpy(atoms->resname,top.atoms.resname,
	 atoms->nres*sizeof(atoms->resname[0]));
  mdatoms = atoms2md(stdlog,&top.atoms,ir.opts.nFreeze,
		     eiMD,0,0,NULL,FALSE,FALSE);  
  fr      = mk_forcerec();
  fprintf(stdlog,"Made forcerec\n");
  calc_nsb(stdlog,&(top.blocks[ebCGS]),1,nsb,0);
  init_forcerec(stdlog,fr,&ir,&top,cr,mdatoms,nsb,box,FALSE,NULL,NULL,FALSE);
  init_nrnb(&nrnb);
  j=0;
  do {
    if (ir.ePBC == epbcXYZ)
      rm_pbc(&top.idef,natoms,box,x,x);
    else if (ir.ePBC == epbcFULL)
      set_pbc(&pbc,box);
    
    if (clust) {
      if (j > clust->maxframe)
	gmx_fatal(FARGS,"There are more frames in the trajectory than in the"
		  " cluster index file (t = %8f)\n",t);
      int my_clust = clust->inv_clust[j];
      range_check(my_clust,0,clust->clust->nr);
      check_viol(stdlog,cr,&(top.idef.il[F_DISRES]),
		 top.idef.iparams,top.idef.functype,
		 x,f,fr,ir.ePBC==epbcFULL ? &pbc : NULL,g,
		 dr_clust,my_clust,isize,index,vvindex,&fcd);
    }
    else
      check_viol(stdlog,cr,&(top.idef.il[F_DISRES]),
		 top.idef.iparams,top.idef.functype,
		 x,f,fr,ir.ePBC==epbcFULL ? &pbc : NULL,g,&dr,0,
		 isize,index,vvindex,&fcd);
    if (bPDB) {
      reset_x(top.atoms.nr,ind_fit,top.atoms.nr,NULL,x,w_rls);
      do_fit(top.atoms.nr,w_rls,x,x);
      if (j == 0) {
	/* Store the first frame of the trajectory as 'characteristic'
	 * for colouring with violations.
	 */
	for(kkk=0; (kkk<top.atoms.nr); kkk++)
	  copy_rvec(x[kkk],xav[kkk]);
      }
    }
    if (!clust) {
      if (isize > 0) {
	fprintf(xvg,"%10g",t);
	for(i=0; (i<isize); i++)
	  fprintf(xvg,"  %10g",vvindex[i]);
	fprintf(xvg,"\n");
      }    
      fprintf(out,  "%10g  %10g\n",t,dr.sumv);
      fprintf(aver, "%10g  %10g\n",t,dr.averv);
      fprintf(maxxv,"%10g  %10g\n",t,dr.maxv);
      fprintf(numv, "%10g  %10d\n",t,dr.nv);
    }
    j++;
  } while (read_next_x(status,&t,natoms,x,box));
  close_trj(status);

  if (clust) {
    dump_clust_stats(stdlog,fcd.disres.nr,&(top.idef.il[F_DISRES]),
		     top.idef.iparams,clust->clust,dr_clust,
		     clust->grpname,isize,index);
  }
  else {
    dump_stats(stdlog,j,fcd.disres.nr,&(top.idef.il[F_DISRES]),
	       top.idef.iparams,&dr,isize,index,
	       bPDB ? (&top.atoms) : NULL);
    if (bPDB) {
      write_sto_conf(opt2fn("-q",NFILE,fnm),
		     "Coloured by average violation in Angstrom",
		     &(top.atoms),xav,NULL,box);
    }
    fclose(out);
    fclose(aver);
    fclose(numv);
    fclose(maxxv);
    if (isize > 0) {
      fclose(xvg);
      do_view(opt2fn("-dr",NFILE,fnm),"-nxy");
    }
    do_view(opt2fn("-dn",NFILE,fnm),"-nxy");
    do_view(opt2fn("-da",NFILE,fnm),"-nxy");
    do_view(opt2fn("-ds",NFILE,fnm),"-nxy");
    do_view(opt2fn("-dm",NFILE,fnm),"-nxy");
  }
  /*thanx(stderr);*/

  fclose(stdlog);
  
  if (gmx_parallel_env)
    gmx_finalize(cr);
  
  return 0;
}
