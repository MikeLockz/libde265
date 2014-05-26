/*
 * H.265 video codec.
 * Copyright (c) 2013-2014 struktur AG, Dirk Farin <farin@struktur.de>
 *
 * This file is part of libde265.
 *
 * libde265 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * libde265 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libde265.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "deblock.h"
#include "util.h"
#include "transform.h"
#include "de265.h"

#include <assert.h>



// 8.7.2.1 for both EDGE_HOR and EDGE_VER at the same time
void markTransformBlockBoundary(de265_image* img, int x0,int y0,
                                int log2TrafoSize,int trafoDepth,
                                int filterLeftCbEdge, int filterTopCbEdge)
{
  logtrace(LogDeblock,"markTransformBlockBoundary(%d,%d, %d,%d, %d,%d)\n",x0,y0,
           log2TrafoSize,trafoDepth, filterLeftCbEdge,filterTopCbEdge);

  int split_transform = img->get_split_transform_flag(x0,y0,trafoDepth);
  if (split_transform) {
    int x1 = x0 + ((1<<log2TrafoSize)>>1);
    int y1 = y0 + ((1<<log2TrafoSize)>>1);

    markTransformBlockBoundary(img,x0,y0,log2TrafoSize-1,trafoDepth+1, filterLeftCbEdge,   filterTopCbEdge);
    markTransformBlockBoundary(img,x1,y0,log2TrafoSize-1,trafoDepth+1, DEBLOCK_FLAG_VERTI, filterTopCbEdge);
    markTransformBlockBoundary(img,x0,y1,log2TrafoSize-1,trafoDepth+1, filterLeftCbEdge,   DEBLOCK_FLAG_HORIZ);
    markTransformBlockBoundary(img,x1,y1,log2TrafoSize-1,trafoDepth+1, DEBLOCK_FLAG_VERTI, DEBLOCK_FLAG_HORIZ);
  }
  else {
    // VER

    for (int k=0;k<(1<<log2TrafoSize);k+=4) {
      img->set_deblk_flags(x0,y0+k, filterLeftCbEdge);
    }

    // HOR

    for (int k=0;k<(1<<log2TrafoSize);k+=4) {
      img->set_deblk_flags(x0+k,y0, filterTopCbEdge);
    }
  }
}



// 8.7.2.2 for both EDGE_HOR and EDGE_VER at the same time
void markPredictionBlockBoundary(de265_image* img, int x0,int y0,
                                 int log2CbSize,
                                 int filterLeftCbEdge, int filterTopCbEdge)
{
  logtrace(LogDeblock,"markPredictionBlockBoundary(%d,%d, %d, %d,%d)\n",x0,y0,
           log2CbSize, filterLeftCbEdge,filterTopCbEdge);

  enum PartMode partMode = img->get_PartMode(x0,y0);

  int cbSize = 1<<log2CbSize;
  int cbSize2 = 1<<(log2CbSize-1);
  int cbSize4 = 1<<(log2CbSize-2);

  switch (partMode) {
  case PART_NxN:
    for (int k=0;k<cbSize;k++) {
      img->set_deblk_flags(x0+cbSize2,y0+k, DEBLOCK_PB_EDGE_VERTI);
      img->set_deblk_flags(x0+k,y0+cbSize2, DEBLOCK_PB_EDGE_HORIZ);
    }
    break;

  case PART_Nx2N:
    for (int k=0;k<cbSize;k++) {
      img->set_deblk_flags(x0+cbSize2,y0+k, DEBLOCK_PB_EDGE_VERTI);
    }
    break;

  case PART_2NxN:
    for (int k=0;k<cbSize;k++) {
      img->set_deblk_flags(x0+k,y0+cbSize2, DEBLOCK_PB_EDGE_HORIZ);
    }
    break;

  case PART_nLx2N:
    for (int k=0;k<cbSize;k++) {
      img->set_deblk_flags(x0+cbSize4,y0+k, DEBLOCK_PB_EDGE_VERTI);
    }
    break;

  case PART_nRx2N:
    for (int k=0;k<cbSize;k++) {
      img->set_deblk_flags(x0+cbSize2+cbSize4,y0+k, DEBLOCK_PB_EDGE_VERTI);
    }
    break;

  case PART_2NxnU:
    for (int k=0;k<cbSize;k++) {
      img->set_deblk_flags(x0+k,y0+cbSize4, DEBLOCK_PB_EDGE_HORIZ);
    }
    break;

  case PART_2NxnD:
    for (int k=0;k<cbSize;k++) {
      img->set_deblk_flags(x0+k,y0+cbSize2+cbSize4, DEBLOCK_PB_EDGE_HORIZ);
    }
    break;

  case PART_2Nx2N:
    // NOP
    break;
  }
}


char derive_edgeFlags(de265_image* img)
{
  const int minCbSize = img->sps.MinCbSizeY;
  char deblocking_enabled=0; // whether deblocking is enabled in some part of the image

  int ctb_mask = (1<<img->sps.Log2CtbSizeY)-1;
  int picWidthInCtbs = img->sps.PicWidthInCtbsY;
  int ctbshift = img->sps.Log2CtbSizeY;

  const pic_parameter_set* pps = &img->pps;

  for (int cb_y=0;cb_y<img->sps.PicHeightInMinCbsY;cb_y++)
    for (int cb_x=0;cb_x<img->sps.PicWidthInMinCbsY;cb_x++)
      {
        int log2CbSize = img->get_log2CbSize_cbUnits(cb_x,cb_y);
        if (log2CbSize==0) {
          continue;
        }

        // we are now at the top corner of a CB

        int x0 = cb_x * minCbSize;
        int y0 = cb_y * minCbSize;

        int x0ctb = x0 >> ctbshift;
        int y0ctb = y0 >> ctbshift;

        // check whether we should filter this slice

        slice_segment_header* shdr = img->get_SliceHeader(x0,y0);

        // check whether to filter left and top edge

        uint8_t filterLeftCbEdge = DEBLOCK_FLAG_VERTI;
        uint8_t filterTopCbEdge  = DEBLOCK_FLAG_HORIZ;
        if (x0 == 0) filterLeftCbEdge = 0;
        if (y0 == 0) filterTopCbEdge  = 0;

        // check for slice and tile boundaries (8.7.2, step 2 in both processes)

        if (x0 && ((x0 & ctb_mask) == 0)) { // left edge at CTB boundary
          if (shdr->slice_loop_filter_across_slices_enabled_flag == 0 &&
              shdr->SliceAddrRS != img->get_SliceHeader(x0-1,y0)->SliceAddrRS)
            {
              filterLeftCbEdge = 0;
            }
          else if (pps->loop_filter_across_tiles_enabled_flag == 0 &&
                   pps->TileIdRS[  x0ctb           +y0ctb*picWidthInCtbs] !=
                   pps->TileIdRS[((x0-1)>>ctbshift)+y0ctb*picWidthInCtbs]) {
            filterLeftCbEdge = 0;
          }
        }

        if (y0 && ((y0 & ctb_mask) == 0)) { // top edge at CTB boundary
          if (shdr->slice_loop_filter_across_slices_enabled_flag == 0 &&
              shdr->SliceAddrRS != img->get_SliceHeader(x0,y0-1)->SliceAddrRS)
            {
              filterTopCbEdge = 0;
            }
          else if (pps->loop_filter_across_tiles_enabled_flag == 0 &&
                   pps->TileIdRS[x0ctb+  y0ctb           *picWidthInCtbs] !=
                   pps->TileIdRS[x0ctb+((y0-1)>>ctbshift)*picWidthInCtbs]) {
            filterTopCbEdge = 0;
          }
        }


        // mark edges

        if (shdr->slice_deblocking_filter_disabled_flag==0) {
          deblocking_enabled=1;

          markTransformBlockBoundary(img, x0,y0, log2CbSize,0,
                                     filterLeftCbEdge, filterTopCbEdge);

          markPredictionBlockBoundary(img, x0,y0, log2CbSize,
                                      filterLeftCbEdge, filterTopCbEdge);
        }
      }

  return deblocking_enabled;
}



// 8.7.2.3 (both, EDGE_VER and EDGE_HOR)
void derive_boundaryStrength(de265_image* img, bool vertical, int yStart,int yEnd,
                             int xStart,int xEnd)
{
  int xIncr = vertical ? 2 : 1;
  int yIncr = vertical ? 1 : 2;
  int xOffs = vertical ? 1 : 0;
  int yOffs = vertical ? 0 : 1;
  int edgeMask = vertical ?
    (DEBLOCK_FLAG_VERTI | DEBLOCK_PB_EDGE_VERTI) :
    (DEBLOCK_FLAG_HORIZ | DEBLOCK_PB_EDGE_HORIZ);
  int transformEdgeMask = vertical ? DEBLOCK_FLAG_VERTI : DEBLOCK_FLAG_HORIZ;

  xEnd = libde265_min(xEnd,img->get_deblk_width());
  yEnd = libde265_min(yEnd,img->get_deblk_height());

  int TUShift = img->sps.Log2MinTrafoSize;
  int TUStride= img->sps.PicWidthInTbsY;

  for (int y=yStart;y<yEnd;y+=yIncr)
    for (int x=xStart;x<xEnd;x+=xIncr) {
      int xDi = x*4;
      int yDi = y*4;

      logtrace(LogDeblock,"%d %d %s = %s\n",xDi,yDi, vertical?"Vertical":"Horizontal",
               (img->get_deblk_flags(xDi,yDi) & edgeMask) ? "edge" : "...");

      uint8_t edgeFlags = img->get_deblk_flags(xDi,yDi);

      if (edgeFlags & edgeMask) {
        bool p_is_intra_pred = (img->get_pred_mode(xDi-xOffs, yDi-yOffs) == MODE_INTRA);
        bool q_is_intra_pred = (img->get_pred_mode(xDi,       yDi      ) == MODE_INTRA);

        int bS;

        if (p_is_intra_pred || q_is_intra_pred) {
          bS = 2;
        }
        else {
          // opposing site
          int xDiOpp = xDi-xOffs;
          int yDiOpp = yDi-yOffs;

          if ((edgeFlags & transformEdgeMask) &&
              (img->get_nonzero_coefficient(xDi   ,yDi) ||
               img->get_nonzero_coefficient(xDiOpp,yDiOpp))) {
            bS = 1;
          }
          else {

            bS = 0;

            const PredVectorInfo* mviP = img->get_mv_info(xDiOpp,yDiOpp);
            const PredVectorInfo* mviQ = img->get_mv_info(xDi   ,yDi);

            slice_segment_header* shdrP = img->get_SliceHeader(xDiOpp,yDiOpp);
            slice_segment_header* shdrQ = img->get_SliceHeader(xDi   ,yDi);

            int refPicP0 = mviP->predFlag[0] ? shdrP->RefPicList[0][ mviP->refIdx[0] ] : -1;
            int refPicP1 = mviP->predFlag[1] ? shdrP->RefPicList[1][ mviP->refIdx[1] ] : -1;
            int refPicQ0 = mviQ->predFlag[0] ? shdrQ->RefPicList[0][ mviQ->refIdx[0] ] : -1;
            int refPicQ1 = mviQ->predFlag[1] ? shdrQ->RefPicList[1][ mviQ->refIdx[1] ] : -1;

            MotionVector mvP0 = mviP->mv[0]; if (!mviP->predFlag[0]) { mvP0.x=mvP0.y=0; }
            MotionVector mvP1 = mviP->mv[1]; if (!mviP->predFlag[1]) { mvP1.x=mvP1.y=0; }
            MotionVector mvQ0 = mviQ->mv[0]; if (!mviQ->predFlag[0]) { mvQ0.x=mvQ0.y=0; }
            MotionVector mvQ1 = mviQ->mv[1]; if (!mviQ->predFlag[1]) { mvQ1.x=mvQ1.y=0; }

            bool samePics = ((refPicP0==refPicQ0 && refPicP1==refPicQ1) ||
                             (refPicP0==refPicQ1 && refPicP1==refPicQ0));

            if (!samePics) {
              bS = 1;
            }
            else {
              int numMV_P = mviP->predFlag[0] + mviP->predFlag[1];
              int numMV_Q = mviQ->predFlag[0] + mviQ->predFlag[1];

              if (numMV_P!=numMV_Q) {
                img->decctx->add_warning(DE265_WARNING_NUMMVP_NOT_EQUAL_TO_NUMMVQ, false);
                img->integrity = INTEGRITY_DECODING_ERRORS;
              }

              // two different reference pictures or only one reference picture
              if (refPicP0 != refPicP1) {

                if (refPicP0 == refPicQ0) {
                  if (abs_value(mvP0.x-mvQ0.x) >= 4 ||
                      abs_value(mvP0.y-mvQ0.y) >= 4 ||
                      abs_value(mvP1.x-mvQ1.x) >= 4 ||
                      abs_value(mvP1.y-mvQ1.y) >= 4) {
                    bS = 1;
                  }
                }
                else {
                  if (abs_value(mvP0.x-mvQ1.x) >= 4 ||
                      abs_value(mvP0.y-mvQ1.y) >= 4 ||
                      abs_value(mvP1.x-mvQ0.x) >= 4 ||
                      abs_value(mvP1.y-mvQ0.y) >= 4) {
                    bS = 1;
                  }
                }
              }
              else {
                assert(refPicQ0==refPicQ1);

                if ((abs_value(mvP0.x-mvQ0.x) >= 4 ||
                     abs_value(mvP0.y-mvQ0.y) >= 4 ||
                     abs_value(mvP1.x-mvQ1.x) >= 4 ||
                     abs_value(mvP1.y-mvQ1.y) >= 4)
                    &&
                    (abs_value(mvP0.x-mvQ1.x) >= 4 ||
                     abs_value(mvP0.y-mvQ1.y) >= 4 ||
                     abs_value(mvP1.x-mvQ0.x) >= 4 ||
                     abs_value(mvP1.y-mvQ0.y) >= 4)) {
                  bS = 1;
                }
              }
            }

            /*
            printf("unimplemented deblocking code for CU at %d;%d\n",xDi,yDi);

            logerror(LogDeblock, "unimplemented code reached (file %s, line %d)\n",
                     __FILE__, __LINE__);
            */
          }
        }

        img->set_deblk_bS(xDi,yDi, bS);
      }
      else {
        img->set_deblk_bS(xDi,yDi, 0);
      }
    }
}


void derive_boundaryStrength_CTB(de265_image* img, bool vertical, int xCtb,int yCtb)
{
  int ctbSize = img->sps.CtbSizeY;
  int deblkSize = ctbSize/4;

  derive_boundaryStrength(img,vertical,
                          yCtb*deblkSize, (yCtb+1)*deblkSize,
                          xCtb*deblkSize, (xCtb+1)*deblkSize);
}


static uint8_t table_8_23_beta[52] = {
   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6, 7, 8,
   9,10,11,12,13,14,15,16,17,18,20,22,24,26,28,30,32,34,36,
  38,40,42,44,46,48,50,52,54,56,58,60,62,64
};

static uint8_t table_8_23_tc[54] = {
   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
   1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4,
   5, 5, 6, 6, 7, 8, 9,10,11,13,14,16,18,20,22,24
};



// 8.7.2.4
void edge_filtering_luma(de265_image* img, bool vertical,
                         int yStart,int yEnd, int xStart,int xEnd)
{
  int xIncr = vertical ? 2 : 1;
  int yIncr = vertical ? 1 : 2;

  const int stride = img->get_image_stride(0);

  int bitDepth_Y = img->sps.BitDepth_Y;

  xEnd = libde265_min(xEnd,img->get_deblk_width());
  yEnd = libde265_min(yEnd,img->get_deblk_height());

  for (int y=yStart;y<yEnd;y+=yIncr)
    for (int x=xStart;x<xEnd;x+=xIncr) {
      int xDi = x*4;
      int yDi = y*4;
      int bS = img->get_deblk_bS(xDi,yDi);

      logtrace(LogDeblock,"deblock POC=%d %c --- x:%d y:%d bS:%d---\n",
               img->PicOrderCntVal,vertical ? 'V':'H',xDi,yDi,bS);

#if 0
      {
        uint8_t* ptr = img->y + stride*yDi + xDi;

        for (int dy=-4;dy<4;dy++) {
          for (int dx=-4;dx<4;dx++) {
            printf("%02x ", ptr[dy*stride + dx]);
            if (dx==-1) printf("| ");
          }
          printf("\n");
          if (dy==-1) printf("-------------------------\n");
        }
      }
#endif

#if 0
      if (!vertical)
        {
          uint8_t* ptr = img->y + stride*yDi + xDi;

          for (int dy=-4;dy<4;dy++) {
            for (int dx=0;dx<4;dx++) {
              printf("%02x ", ptr[dy*stride + dx]);
              if (dx==-1) printf("| ");
            }
            printf("\n");
            if (dy==-1) printf("-------------------------\n");
          }
        }
#endif

      if (bS>0) {

        // 8.7.2.4.3

        uint8_t* ptr = img->get_image_plane_at_pos(0, xDi,yDi);

        uint8_t q[4][4], p[4][4];
        for (int k=0;k<4;k++)
          for (int i=0;i<4;i++)
            {
              if (vertical) {
                q[k][i] = ptr[ i  +k*stride];
                p[k][i] = ptr[-i-1+k*stride];
              }
              else {
                q[k][i] = ptr[k + i   *stride];
                p[k][i] = ptr[k -(i+1)*stride];
              }
            }

#if 0
        for (int k=0;k<4;k++)
          {
            for (int i=0;i<4;i++)
              {
                printf("%02x ", p[k][3-i]);
              }

            printf("| ");

            for (int i=0;i<4;i++)
              {
                printf("%02x ", q[k][i]);
              }
            printf("\n");
          }
#endif


        int QP_Q = img->get_QPY(xDi,yDi);
        int QP_P = (vertical ?
                    img->get_QPY(xDi-1,yDi) :
                    img->get_QPY(xDi,yDi-1) );
        int qP_L = (QP_Q+QP_P+1)>>1;

        logtrace(LogDeblock,"QP: %d & %d -> %d\n",QP_Q,QP_P,qP_L);

        int sliceIndexQ00 = img->get_SliceHeaderIndex(xDi,yDi);
        int beta_offset = img->slices[sliceIndexQ00]->slice_beta_offset;
        int tc_offset   = img->slices[sliceIndexQ00]->slice_tc_offset;

        int Q_beta = Clip3(0,51, qP_L + beta_offset);
        int betaPrime = table_8_23_beta[Q_beta];
        int beta = betaPrime * (1<<(bitDepth_Y - 8));

        int Q_tc = Clip3(0,53, qP_L + 2*(bS-1) + tc_offset);
        int tcPrime = table_8_23_tc[Q_tc];
        int tc = tcPrime * (1<<(bitDepth_Y - 8));

        logtrace(LogDeblock,"beta: %d (%d)  tc: %d (%d)\n",beta,beta_offset, tc,tc_offset);

        int dE=0, dEp=0, dEq=0;

        if (vertical || !vertical) {
          int dp0 = abs_value(p[0][2] - 2*p[0][1] + p[0][0]);
          int dp3 = abs_value(p[3][2] - 2*p[3][1] + p[3][0]);
          int dq0 = abs_value(q[0][2] - 2*q[0][1] + q[0][0]);
          int dq3 = abs_value(q[3][2] - 2*q[3][1] + q[3][0]);

          int dpq0 = dp0 + dq0;
          int dpq3 = dp3 + dq3;

          int dp = dp0 + dp3;
          int dq = dq0 + dq3;
          int d  = dpq0+ dpq3;

          if (d<beta) {
            //int dpq = 2*dpq0;
            bool dSam0 = (2*dpq0 < (beta>>2) &&
                          abs_value(p[0][3]-p[0][0])+abs_value(q[0][0]-q[0][3]) < (beta>>3) &&
                          abs_value(p[0][0]-q[0][0]) < ((5*tc+1)>>1));

            bool dSam3 = (2*dpq3 < (beta>>2) &&
                          abs_value(p[3][3]-p[3][0])+abs_value(q[3][0]-q[3][3]) < (beta>>3) &&
                          abs_value(p[3][0]-q[3][0]) < ((5*tc+1)>>1));

            if (dSam0 && dSam3) {
              dE=2;
            }
            else {
              dE=1;
            }

            if (dp < ((beta + (beta>>1))>>3)) { dEp=1; }
            if (dq < ((beta + (beta>>1))>>3)) { dEq=1; }

            logtrace(LogDeblock,"dE:%d dEp:%d dEq:%d\n",dE,dEp,dEq);
          }
        }
        else {
          // TODO
          assert(0);
        }


        // 8.7.2.4.4

        if (dE != 0) {
          bool filterP = true;
          bool filterQ = true;

          if (vertical) {
            if (img->sps.pcm_loop_filter_disable_flag && img->get_pcm_flag(xDi-1,yDi)) filterP=false;
            if (img->get_cu_transquant_bypass(xDi-1,yDi)) filterP=false;

            if (img->sps.pcm_loop_filter_disable_flag && img->get_pcm_flag(xDi,yDi)) filterQ=false;
            if (img->get_cu_transquant_bypass(xDi,yDi)) filterQ=false;
          }
          else {
            if (img->sps.pcm_loop_filter_disable_flag && img->get_pcm_flag(xDi,yDi-1)) filterP=false;
            if (img->get_cu_transquant_bypass(xDi,yDi-1)) filterP=false;

            if (img->sps.pcm_loop_filter_disable_flag && img->get_pcm_flag(xDi,yDi)) filterQ=false;
            if (img->get_cu_transquant_bypass(xDi,yDi)) filterQ=false;
          }

          for (int k=0;k<4;k++) {
            //int nDp,nDq;

            logtrace(LogDeblock,"line:%d\n",k);

            const uint8_t p0 = p[k][0];
            const uint8_t p1 = p[k][1];
            const uint8_t p2 = p[k][2];
            const uint8_t p3 = p[k][3];
            const uint8_t q0 = q[k][0];
            const uint8_t q1 = q[k][1];
            const uint8_t q2 = q[k][2];
            const uint8_t q3 = q[k][3];

            if (dE==2) {
              // strong filtering

              //nDp=nDq=3;

              uint8_t pnew[3],qnew[3];
              pnew[0] = Clip3(p0-2*tc,p0+2*tc, (p2 + 2*p1 + 2*p0 + 2*q0 + q1 +4)>>3);
              pnew[1] = Clip3(p1-2*tc,p1+2*tc, (p2 + p1 + p0 + q0+2)>>2);
              pnew[2] = Clip3(p2-2*tc,p2+2*tc, (2*p3 + 3*p2 + p1 + p0 + q0 + 4)>>3);
              qnew[0] = Clip3(q0-2*tc,q0+2*tc, (p1+2*p0+2*q0+2*q1+q2+4)>>3);
              qnew[1] = Clip3(q1-2*tc,q1+2*tc, (p0+q0+q1+q2+2)>>2);
              qnew[2] = Clip3(q2-2*tc,q2+2*tc, (p0+q0+q1+3*q2+2*q3+4)>>3);

              logtrace(LogDeblock,"strong filtering\n");

              if (vertical) {
                for (int i=0;i<3;i++) {
                  if (filterP) { ptr[-i-1+k*stride] = pnew[i]; }
                  if (filterQ) { ptr[ i + k*stride] = qnew[i]; }
                }

                // ptr[-1+k*stride] = ptr[ 0+k*stride] = 200;
              }
              else {
                for (int i=0;i<3;i++) {
                  if (filterP) { ptr[ k -(i+1)*stride] = pnew[i]; }
                  if (filterQ) { ptr[ k + i   *stride] = qnew[i]; }
                }
              }
            }
            else {
              // weak filtering

              //nDp=nDq=0;

              int delta = (9*(q0-p0) - 3*(q1-p1) + 8)>>4;
              logtrace(LogDeblock,"delta=%d, tc=%d\n",delta,tc);

              if (abs_value(delta) < tc*10) {

                delta = Clip3(-tc,tc,delta);
                logtrace(LogDeblock," deblk + %d;%d [%02x->%02x]  - %d;%d [%02x->%02x] delta:%d\n",
                         vertical ? xDi-1 : xDi+k,
                         vertical ? yDi+k : yDi-1, p0,Clip1_8bit(p0+delta),
                         vertical ? xDi   : xDi+k,
                         vertical ? yDi+k : yDi, q0,Clip1_8bit(q0-delta),
                         delta);

                if (vertical) {
                  if (filterP) { ptr[-0-1+k*stride] = Clip1_8bit(p0+delta); }
                  if (filterQ) { ptr[ 0  +k*stride] = Clip1_8bit(q0-delta); }
                }
                else {
                  if (filterP) { ptr[ k -1*stride] = Clip1_8bit(p0+delta); }
                  if (filterQ) { ptr[ k +0*stride] = Clip1_8bit(q0-delta); }
                }

                //ptr[ 0+k*stride] = 200;

                if (dEp==1 && filterP) {
                  int delta_p = Clip3(-(tc>>1), tc>>1, (((p2+p0+1)>>1)-p1+delta)>>1);

                  logtrace(LogDeblock," deblk dEp %d;%d delta:%d\n",
                           vertical ? xDi-2 : xDi+k,
                           vertical ? yDi+k : yDi-2,
                           delta_p);

                  if (vertical) { ptr[-1-1+k*stride] = Clip1_8bit(p1+delta_p); }
                  else          { ptr[ k  -2*stride] = Clip1_8bit(p1+delta_p); }
                }

                if (dEq==1 && filterQ) {
                  int delta_q = Clip3(-(tc>>1), tc>>1, (((q2+q0+1)>>1)-q1-delta)>>1);

                  logtrace(LogDeblock," delkb dEq %d;%d delta:%d\n",
                           vertical ? xDi+1 : xDi+k,
                           vertical ? yDi+k : yDi+1,
                           delta_q);

                  if (vertical) { ptr[ 1  +k*stride] = Clip1_8bit(q1+delta_q); }
                  else          { ptr[ k  +1*stride] = Clip1_8bit(q1+delta_q); }
                }

                //nDp = dEp+1;
                //nDq = dEq+1;

                //logtrace(LogDeblock,"weak filtering (%d:%d)\n",nDp,nDq);
              }
            }
          }
        }
      }
    }
}


void edge_filtering_luma_CTB(de265_image* img, bool vertical, int xCtb,int yCtb)
{
  int ctbSize = img->sps.CtbSizeY;
  int deblkSize = ctbSize/4;

  edge_filtering_luma(img,vertical,
                      yCtb*deblkSize, (yCtb+1)*deblkSize,
                      xCtb*deblkSize, (xCtb+1)*deblkSize);
}




// 8.7.2.4
void edge_filtering_chroma(de265_image* img, bool vertical, int yStart,int yEnd,
                           int xStart,int xEnd)
{
  int xIncr = vertical ? 4 : 2;
  int yIncr = vertical ? 2 : 4;

  const int stride = img->get_image_stride(1);

  xEnd = libde265_min(xEnd,img->get_deblk_width());
  yEnd = libde265_min(yEnd,img->get_deblk_height());

  for (int y=yStart;y<yEnd;y+=yIncr)
    for (int x=xStart;x<xEnd;x+=xIncr) {
      int xDi = x*2;
      int yDi = y*2;
      int bS = img->get_deblk_bS(2*xDi,2*yDi);

      if (bS>1) {
        // 8.7.2.4.5

        for (int cplane=0;cplane<2;cplane++) {
          int cQpPicOffset = (cplane==0 ?
                              img->pps.pic_cb_qp_offset :
                              img->pps.pic_cr_qp_offset);

          uint8_t* ptr = img->get_image_plane_at_pos(cplane+1, xDi,yDi);

          uint8_t p[2][4];
          uint8_t q[2][4];

          logtrace(LogDeblock,"-%s- %d %d\n",cplane==0 ? "Cb" : "Cr",xDi,yDi);

          for (int i=0;i<2;i++)
            for (int k=0;k<4;k++)
              {
                if (vertical) {
                  q[i][k] = ptr[ i  +k*stride];
                  p[i][k] = ptr[-i-1+k*stride];
                }
                else {
                  q[i][k] = ptr[k + i   *stride];
                  p[i][k] = ptr[k -(i+1)*stride];
                }
              }

#if 0
          for (int k=0;k<4;k++)
            {
              for (int i=0;i<2;i++)
                {
                  printf("%02x ", p[1-i][k]);
                }

              printf("| ");

              for (int i=0;i<2;i++)
                {
                  printf("%02x ", q[i][k]);
                }
              printf("\n");
            }
#endif

          int QP_Q = img->get_QPY(2*xDi,2*yDi);
          int QP_P = (vertical ?
                      img->get_QPY(2*xDi-1,2*yDi) :
                      img->get_QPY(2*xDi,2*yDi-1));
          int qP_i = ((QP_Q+QP_P+1)>>1) + cQpPicOffset;
          int QP_C = table8_22(qP_i);

          //printf("POC=%d\n",ctx->img->PicOrderCntVal);
          logtrace(LogDeblock,"%d %d: ((%d+%d+1)>>1) + %d = qP_i=%d  (QP_C=%d)\n",
                   2*xDi,2*yDi, QP_Q,QP_P,cQpPicOffset,qP_i,QP_C);
          
          int sliceIndexQ00 = img->get_SliceHeaderIndex(2*xDi,2*yDi);
          int tc_offset   = img->slices[sliceIndexQ00]->slice_tc_offset;

          int Q = Clip3(0,53, QP_C + 2*(bS-1) + tc_offset);

          int tcPrime = table_8_23_tc[Q];
          int tc = tcPrime * (1<<(img->sps.BitDepth_C - 8));

          logtrace(LogDeblock,"tc_offset=%d Q=%d tc'=%d tc=%d\n",tc_offset,Q,tcPrime,tc);

          if (vertical) {
            bool filterP = true;
            if (img->sps.pcm_loop_filter_disable_flag && img->get_pcm_flag(2*xDi-1,2*yDi)) filterP=false;
            if (img->get_cu_transquant_bypass(2*xDi-1,2*yDi)) filterP=false;

            bool filterQ = true;
            if (img->sps.pcm_loop_filter_disable_flag && img->get_pcm_flag(2*xDi,2*yDi)) filterQ=false;
            if (img->get_cu_transquant_bypass(2*xDi,2*yDi)) filterQ=false;


            for (int k=0;k<4;k++) {
              int delta = Clip3(-tc,tc, ((((q[0][k]-p[0][k])<<2)+p[1][k]-q[1][k]+4)>>3));
              logtrace(LogDeblock,"delta=%d\n",delta);
              if (filterP) { ptr[-1+k*stride] = Clip1_8bit(p[0][k]+delta); }
              if (filterQ) { ptr[ 0+k*stride] = Clip1_8bit(q[0][k]-delta); }
            }
          }
          else {
            bool filterP = true;
            if (img->sps.pcm_loop_filter_disable_flag && img->get_pcm_flag(2*xDi,2*yDi-1)) filterP=false;
            if (img->get_cu_transquant_bypass(2*xDi,2*yDi-1)) filterP=false;

            bool filterQ = true;
            if (img->sps.pcm_loop_filter_disable_flag && img->get_pcm_flag(2*xDi,2*yDi)) filterQ=false;
            if (img->get_cu_transquant_bypass(2*xDi,2*yDi)) filterQ=false;

            for (int k=0;k<4;k++) {
              int delta = Clip3(-tc,tc, ((((q[0][k]-p[0][k])<<2)+p[1][k]-q[1][k]+4)>>3));
              if (filterP) { ptr[ k-1*stride] = Clip1_8bit(p[0][k]+delta); }
              if (filterQ) { ptr[ k+0*stride] = Clip1_8bit(q[0][k]-delta); }
            }
          }
        }
      }
    }
}

void edge_filtering_chroma_CTB(de265_image* img, bool vertical, int xCtb,int yCtb)
{
  int ctbSize = img->sps.CtbSizeY;
  int deblkSize = ctbSize/4;

  edge_filtering_chroma(img,vertical,
                        yCtb*deblkSize, (yCtb+1)*deblkSize,
                        xCtb*deblkSize, (xCtb+1)*deblkSize);
}




class thread_task_deblock : public thread_task
{
public:
  struct de265_image* img;
  int first;  // stripe row
  int last;
  int ctb_x,ctb_y;
  bool vertical;

  virtual void work();
};


void thread_task_deblock::work()
{
  state = Running;
  img->thread_run();

  int xStart=0;
  int xEnd = img->get_deblk_width();

  derive_boundaryStrength(img, vertical, first,last, xStart,xEnd);
  edge_filtering_luma    (img, vertical, first,last, xStart,xEnd);
  edge_filtering_chroma  (img, vertical, first,last, xStart,xEnd);

  state = Finished;
  img->thread_finishes();
}


#if 0
static void thread_deblock_ctb(void* d)
{
  struct thread_task_deblock* data = (struct thread_task_deblock*)d;
  struct decoder_context* ctx = data->ctx;

  derive_boundaryStrength_CTB(ctx, data->vertical, data->ctb_x,data->ctb_y);
  edge_filtering_luma_CTB    (ctx, data->vertical, data->ctb_x,data->ctb_y);
  edge_filtering_chroma_CTB  (ctx, data->vertical, data->ctb_x,data->ctb_y);

  ctx->img->decrease_pending_tasks(1);
}
#endif

/*
static void thread_deblock_ctb_row(void* d)
{
  struct thread_task_deblock* data = (struct thread_task_deblock*)d;
  struct decoder_context* ctx = data->ctx;

  for (int x=0;x<ctx->current_sps->PicWidthInCtbsY;x++) {
    derive_boundaryStrength_CTB(ctx, data->vertical, x,data->ctb_y);
    edge_filtering_luma_CTB    (ctx, data->vertical, x,data->ctb_y);
    edge_filtering_chroma_CTB  (ctx, data->vertical, x,data->ctb_y);
  }
}

static void thread_deblock_full_ctb_row(void* d)
{
  struct thread_task_deblock* data = (struct thread_task_deblock*)d;
  struct decoder_context* ctx = data->ctx;

  de265_image* img = ctx->img;

  int ctbSize = ctx->current_sps->CtbSizeY;
  int deblkSize = ctbSize/4;

  int xStart=0;
  int xEnd = img->deblk_width;

  int yStart =  data->ctb_y   *deblkSize;
  int yEnd   = (data->ctb_y+1)*deblkSize;

  derive_boundaryStrength(ctx, data->vertical, yStart,yEnd, xStart,xEnd);
  edge_filtering_luma    (ctx, data->vertical, yStart,yEnd, xStart,xEnd);
  edge_filtering_chroma  (ctx, data->vertical, yStart,yEnd, xStart,xEnd);
}
*/


void apply_deblocking_filter(de265_image* img) // decoder_context* ctx)
{
  char enabled_deblocking = derive_edgeFlags(img);
  decoder_context* ctx = img->decctx;


  if (enabled_deblocking)
    {
      if (ctx->get_num_worker_threads()==0) {  // TMP HACK / TODO / switched off multi-core

        // vertical filtering

        logtrace(LogDeblock,"VERTICAL\n");
        derive_boundaryStrength(img, true ,0,img->get_deblk_height(),0,img->get_deblk_width());
        edge_filtering_luma    (img, true ,0,img->get_deblk_height(),0,img->get_deblk_width());
        edge_filtering_chroma  (img, true ,0,img->get_deblk_height(),0,img->get_deblk_width());

#if 0
          char buf[1000];
          sprintf(buf,"lf-after-V-%05d.yuv", ctx->img->PicOrderCntVal);
          write_picture_to_file(ctx->img, buf);
#endif

        // horizontal filtering

        logtrace(LogDeblock,"HORIZONTAL\n");
        derive_boundaryStrength(img, false ,0,img->get_deblk_height(),0,img->get_deblk_width());
        edge_filtering_luma    (img, false ,0,img->get_deblk_height(),0,img->get_deblk_width());
        edge_filtering_chroma  (img, false ,0,img->get_deblk_height(),0,img->get_deblk_width());

#if 0
        sprintf(buf,"lf-after-H-%05d.yuv", ctx->img->PicOrderCntVal);
        write_picture_to_file(ctx->img, buf);
#endif
      }
      else {
#if 1
        for (int pass=0;pass<2;pass++) {

          int numStripes= ctx->get_num_worker_threads() * 4; // TODO: what is a good number of stripes?

	  std::vector<thread_task_deblock> tasks(numStripes);

          img->thread_start(numStripes);

          for (int i=0;i<numStripes;i++)
            {
              int ys =  i   *img->get_deblk_height()/numStripes;
              int ye = (i+1)*img->get_deblk_height()/numStripes;

              // required because multi-threading might cut odd strips
              ys &= ~3;
              if (i != numStripes-1) ye &= ~3;

              tasks[i].img   = img;
              tasks[i].first = ys;
              tasks[i].last  = ye;
              tasks[i].vertical = (pass==0);

              add_task(&ctx->thread_pool, &tasks[i]);
            }

          img->wait_for_completion();
        }
#endif
#if 0
        for (int pass=0;pass<2;pass++)
          {
            thread_task task;

            task.work_routine = thread_deblock_ctb;

            //ctx->thread_pool.tasks_pending = ctx->current_sps->PicSizeInCtbsY;
            increase_pending_tasks(img, img_>sps.PicSizeInCtbsY);

            for (int y=0;y<img->sps.PicHeightInCtbsY;y++)
              for (int x=0;x<img->sps.PicWidthInCtbsY;x++)
                {
                  task.data.task_deblock.img   = img;
                  task.data.task_deblock.ctb_x = x;
                  task.data.task_deblock.ctb_y = y;
                  task.data.task_deblock.vertical = (pass==0);
                
                  add_task(&ctx->thread_pool, &task);
                }

            wait_for_completion(img);
          }
#endif
#if 0
        for (int pass=0;pass<2;pass++)
          {
            thread_task task;

            task.work_routine = thread_deblock_ctb_row;

            ctx->thread_pool.tasks_pending = img->sps.PicHeightInCtbsY;

            for (int y=0;y<img->sps.PicHeightInCtbsY;y++)
              //for (int x=0;x<ctx->current_sps->PicWidthInCtbsY;x++)
                {
                  task.data.task_deblock.img   = img;
                  //task.data.task_deblock.ctb_x = x;
                  task.data.task_deblock.ctb_y = y;
                  task.data.task_deblock.vertical = (pass==0);
                
                  add_task(&ctx->thread_pool, &task);
                }
          }
#endif
#if 0
        for (int pass=0;pass<2;pass++)
          {
            thread_task task;

            task.work_routine = thread_deblock_full_ctb_row;

            ctx->thread_pool.tasks_pending = img->sps.PicHeightInCtbsY;

            for (int y=0;y<img->sps.PicHeightInCtbsY;y++)
              //for (int x=0;x<ctx->current_sps->PicWidthInCtbsY;x++)
                {
                  task.data.task_deblock.img   = img;
                  //task.data.task_deblock.ctb_x = x;
                  task.data.task_deblock.ctb_y = y;
                  task.data.task_deblock.vertical = (pass==0);
                
                  add_task(&ctx->thread_pool, &task);
                }
          }
#endif
      }
    }
}
