#ifndef MATCHER_H
#define MATCHER_H

//
// Written by Martin Steinegger & Maria Hauser, mhauser@genzentrum.lmu.de
//
// Calls SSE2 parallelized calculation of Smith-Waterman alignment and non-parallelized traceback afterwards.
//

#include <cfloat>
#include <algorithm>
#include <vector>

#include "Sequence.h"
#include "BaseMatrix.h"
#include "smith_waterman_sse2.h"
#include "BlastScoreUtils.h"

class Matcher{

public:


    struct result_t {
        unsigned int dbKey;
        int score;
        float qcov;
        float dbcov;
        float seqId;
        double eval;
        unsigned int alnLength;
        unsigned int qStartPos;
        unsigned int qEndPos;
        unsigned int qLen;
        unsigned int dbStartPos;
        unsigned int dbEndPos;
        unsigned int dbLen;
        std::string backtrace;
        result_t(unsigned int dbkey,int score,
                 float qcov, float dbcov,
                 float seqId, double eval,
                 unsigned int alnLength,
                 unsigned int qStartPos,
                 unsigned int qEndPos,
                 unsigned int qLen,
                 unsigned int dbStartPos,
                 unsigned int dbEndPos,
                 unsigned int dbLen,
                 std::string backtrace) : dbKey(dbkey), score(score), qcov(qcov),
                                          dbcov(dbcov), seqId(seqId), eval(eval), alnLength(alnLength),
                                          qStartPos(qStartPos), qEndPos(qEndPos), qLen(qLen),
                                          dbStartPos(dbStartPos), dbEndPos(dbEndPos), dbLen(dbLen),
                                          backtrace(backtrace) {};
    };

    Matcher(int maxSeqLen, BaseMatrix *m, size_t dbLen, size_t dbSize, bool aaBiasCorrection);

    ~Matcher();

    // run SSE2 parallelized Smith-Waterman alignment calculation and traceback
    result_t getSWResult(Sequence* dbSeq,const size_t seqDbSize,const double evalThr, const unsigned int mode);

    // need for sorting the results
    static bool compareHits (result_t first, result_t second){ return (first.eval < second.eval); }

    // map new query into memory (create profile, ...)
    void initQuery(Sequence* query);

    static float computeCov(unsigned int startPos, unsigned int endPos, unsigned int len);

    static std::vector<result_t> readAlignmentResults(char *data);

private:

    // calculate the query profile for SIMD registers processing 8 elements
    int maxSeqLen;

    // holds values of the current active query
    Sequence * currentQuery;

    // aligner Class
    SmithWaterman * aligner;
    // parameter for alignment
    const unsigned short GAP_OPEN = 11;
    const unsigned short GAP_EXTEND = 1;
    // substitution matrix
    BaseMatrix* m;
    // byte version of substitution matrix
    int8_t * tinySubMat;
    // set substituion matrix
    void setSubstitutionMatrix(BaseMatrix *m);

    // BLAST statistics
    double *kmnByLen; // contains Kmn for
    double logKLog2; // log(k)/log(2)
    double lambdaLog2; //lambda/log(2)
    double lambda;

    static size_t computeAlnLength(size_t anEnd, size_t start, size_t dbEnd, size_t dbStart);

    float estimateSeqIdByScorePerCol(uint16_t score, unsigned int qLen, unsigned int tLen);
};

#endif
