#include "DistanceCalculator.h"
#include "Util.h"
#include "Parameters.h"
#include "Matcher.h"
#include "Debug.h"
#include "DBReader.h"
#include "DBWriter.h"
#include "QueryMatcher.h"
#include "CovSeqidQscPercMinDiag.out.h"
#include "CovSeqidQscPercMinDiagTargetCov.out.h"
#include "QueryMatcher.h"
#include "NucleotideMatrix.h"
#include "IndexReader.h"

#ifdef OPENMP
#include <omp.h>
#endif

float parsePrecisionLib(const std::string &scoreFile, double targetSeqid, double targetCov, double targetPrecision) {
    std::stringstream in(scoreFile);
    std::string line;
    // find closest lower seq. id in a grid of size 5
    int intTargetSeqid = static_cast<int>((targetSeqid + 0.0001) * 100);
    int seqIdRest = (intTargetSeqid % 5);
    targetSeqid = static_cast<float>(intTargetSeqid - seqIdRest) / 100;
    // find closest lower cov. id in a grid of size 10
    targetCov = static_cast<float>(static_cast<int>((targetCov + 0.0001) * 10)) / 10;
    while (std::getline(in, line)) {
        std::vector<std::string> values = Util::split(line, " ");
        float cov = strtod(values[0].c_str(), NULL);
        float seqid = strtod(values[1].c_str(), NULL);
        float scorePerCol = strtod(values[2].c_str(), NULL);
        float precision = strtod(values[3].c_str(), NULL);
        if (MathUtil::AreSame(cov, targetCov) && MathUtil::AreSame(seqid, targetSeqid) && precision >= targetPrecision) {
            return scorePerCol;
        }
    }
    Debug(Debug::WARNING) << "Could not find any score per column for cov "
                          << targetCov << " seq.id. " << targetSeqid << ". No hit will be filtered.\n";

    return 0;
}

int doRescorediagonal(Parameters &par,
                      DBWriter &resultWriter,
                      DBReader<unsigned int> &resultReader,
              const size_t dbFrom, const size_t dbSize) {


    IndexReader * qDbrIdx = NULL;
    DBReader<unsigned int> * qdbr = NULL;
    DBReader<unsigned int> * tdbr = NULL;
    bool touch = (par.preloadMode != Parameters::PRELOAD_MODE_MMAP);
    IndexReader * tDbrIdx = new IndexReader(par.db2, par.threads, IndexReader::SEQUENCES, (touch) ? (IndexReader::PRELOAD_INDEX | IndexReader::PRELOAD_DATA) : 0 );
    int querySeqType = 0;
    tdbr = tDbrIdx->sequenceReader;
    int targetSeqType = tDbrIdx->getDbtype();
    bool sameQTDB = (par.db2.compare(par.db1) == 0);
    if (sameQTDB == true) {
        qDbrIdx = tDbrIdx;
        qdbr = tdbr;
        querySeqType = targetSeqType;
    } else {
        // open the sequence, prefiltering and output databases
        qDbrIdx = new IndexReader(par.db1, par.threads,  IndexReader::SEQUENCES, (touch) ? IndexReader::PRELOAD_INDEX : 0);
        qdbr = qDbrIdx->sequenceReader;
        querySeqType = qdbr->getDbtype();
    }

    if(resultReader.isSortedByOffset() && qdbr->isSortedByOffset()){
        qdbr->setSequentialAdvice();
    }

    BaseMatrix *subMat;
    if (Parameters::isEqualDbtype(querySeqType, Parameters::DBTYPE_NUCLEOTIDES)) {
        subMat = new NucleotideMatrix(par.scoringMatrixFile.c_str(), 1.0, 0.0);
    } else {
        // keep score bias at 0.0 (improved ROC)
        subMat = new SubstitutionMatrix(par.scoringMatrixFile.c_str(), 2.0, 0.0);
    }

    SubstitutionMatrix::FastMatrix fastMatrix = SubstitutionMatrix::createAsciiSubMat(*subMat);


    float scorePerColThr = 0.0;
    if (par.filterHits) {
        if (par.rescoreMode == Parameters::RESCORE_MODE_HAMMING) {
            Debug(Debug::WARNING) << "HAMMING distance can not be used to filter hits. Using --rescore-mode 1\n";
            par.rescoreMode = Parameters::RESCORE_MODE_SUBSTITUTION;
        }

        std::string libraryString = (par.covMode == Parameters::COV_MODE_BIDIRECTIONAL)
                                    ? std::string((const char*)CovSeqidQscPercMinDiag_out, CovSeqidQscPercMinDiag_out_len)
                                    : std::string((const char*)CovSeqidQscPercMinDiagTargetCov_out, CovSeqidQscPercMinDiagTargetCov_out_len);
        scorePerColThr = parsePrecisionLib(libraryString, par.seqIdThr, par.covThr, 0.99);
    }
    bool reversePrefilterResult = (Parameters::isEqualDbtype(resultReader.getDbtype(), Parameters::DBTYPE_PREFILTER_REV_RES));
    EvalueComputation evaluer(tdbr->getAminoAcidDBSize(), subMat);
    DistanceCalculator globalAliStat;
    if (par.globalAlignment) {
        globalAliStat.prepareGlobalAliParam(*subMat);
    }

    Debug(Debug::INFO) << "Result database: " << par.db4 << "\n";
    size_t totalMemory = Util::getTotalSystemMemory();
    size_t flushSize = 100000000;
    if (totalMemory > resultReader.getTotalDataSize()) {
        flushSize = resultReader.getSize();
    }
    size_t iterations = static_cast<int>(ceil(static_cast<double>(dbSize) / static_cast<double>(flushSize)));
    for (size_t i = 0; i < iterations; i++) {
        size_t start = dbFrom + (i * flushSize);
        size_t bucketSize = std::min(dbSize - (i * flushSize), flushSize);
#pragma omp parallel
        {
            unsigned int thread_idx = 0;
#ifdef OPENMP
            thread_idx = (unsigned int) omp_get_thread_num();
#endif
            char buffer[1024 + 32768];
            std::string resultBuffer;
            resultBuffer.reserve(1000000);
            std::string queryBuffer;
            queryBuffer.reserve(32768);
            std::vector<Matcher::result_t> alnResults;
            alnResults.reserve(300);
            std::vector<hit_t> shortResults;
            shortResults.reserve(300);
            char *queryRevSeq = NULL;
            int queryRevSeqLen = par.maxSeqLen;
            if (reversePrefilterResult == true) {
                queryRevSeq = new char[queryRevSeqLen];
            }
#pragma omp for schedule(dynamic, 1)
            for (size_t id = start; id < (start + bucketSize); id++) {
                Debug::printProgress(id);

                char *data = resultReader.getData(id, thread_idx);
                size_t queryKey = resultReader.getDbKey(id);
                char *querySeq = NULL;
                unsigned int queryId = UINT_MAX;
                int queryLen = -1;
                if(*data !=  '\0'){
                    queryId = qdbr->getId(queryKey);
                    querySeq = qdbr->getData(queryId, thread_idx);
                    queryLen = std::max(0, static_cast<int>(qdbr->getSeqLens(queryId)) - 2);
                    if(queryLen > queryRevSeqLen){
                        delete [] queryRevSeq;
                        queryRevSeq = new char[queryLen];
                        queryRevSeqLen = queryLen;
                    }
                    if (reversePrefilterResult == true) {
                        NucleotideMatrix *nuclMatrix = (NucleotideMatrix *) subMat;
                        for (int pos = queryLen - 1; pos > -1; pos--) {
                            int res = subMat->aa2int[static_cast<int>(querySeq[pos])];
                            queryRevSeq[(queryLen - 1) - pos] = subMat->int2aa[nuclMatrix->reverseResidue(res)];
                        }
                    }
                    if (sameQTDB && qdbr->isCompressed()) {
                        queryBuffer.clear();
                        queryBuffer.append(querySeq, queryLen);
                        querySeq = (char *) queryBuffer.c_str();
                    }
                }

//                if(par.rescoreMode != Parameters::RESCORE_MODE_HAMMING){
//                    query.mapSequence(id, queryId, querySeq);
//                    queryLen = query.L;
//                }else{
                // -2 because of \n\0 in sequenceDB
//                }

                std::vector<hit_t> results = QueryMatcher::parsePrefilterHits(data);
                for (size_t entryIdx = 0; entryIdx < results.size(); entryIdx++) {
                    char *querySeqToAlign = querySeq;
                    bool isReverse = false;
                    if (reversePrefilterResult) {
                        if (results[entryIdx].prefScore == 1) {
                            querySeqToAlign = queryRevSeq;
                            isReverse=true;
                        }
                    }

                    unsigned int targetId = tdbr->getId(results[entryIdx].seqId);
                    const bool isIdentity = (queryId == targetId && (par.includeIdentity || sameQTDB)) ? true : false;
                    char *targetSeq = tdbr->getData(targetId, thread_idx);
                    int dbLen = std::max(0, static_cast<int>(tdbr->getSeqLens(targetId)) - 2);

                    float queryLength = static_cast<float>(queryLen);
                    float targetLength = static_cast<float>(dbLen);
                    if (Util::canBeCovered(par.covThr, par.covMode, queryLength, targetLength) == false) {
                        continue;
                    }
                    DistanceCalculator::LocalAlignment alignment = DistanceCalculator::computeUngappedAlignment(
                                                                                      querySeqToAlign, queryLen, targetSeq, targetLength,
                                                                                      results[entryIdx].diagonal, fastMatrix.matrix, par.rescoreMode);
                    unsigned int distanceToDiagonal = alignment.distToDiagonal;
                    int diagonalLen = alignment.diagonalLen;
                    int distance = alignment.score;
                    int diagonal = alignment.diagonal;
                    double seqId = 0;
                    double evalue = 0.0;
                    int bitScore = 0;
                    int alnLen = 0;
                    float targetCov = static_cast<float>(diagonalLen) / static_cast<float>(dbLen);
                    float queryCov = static_cast<float>(diagonalLen) / static_cast<float>(queryLen);

                    Matcher::result_t result;
                    if (par.rescoreMode == Parameters::RESCORE_MODE_HAMMING) {
                        int idCnt = (static_cast<float>(distance));
                        seqId = Util::computeSeqId(par.seqIdMode, idCnt, queryLen, dbLen, diagonalLen);
                        alnLen = diagonalLen;
                    } else if (par.rescoreMode == Parameters::RESCORE_MODE_SUBSTITUTION ||
                               par.rescoreMode == Parameters::RESCORE_MODE_ALIGNMENT) {
                        //seqId = exp(static_cast<float>(distance) / static_cast<float>(diagonalLen));
                        if (par.globalAlignment) {
                            // FIXME: value is never written to file
                            seqId = globalAliStat.getPvalGlobalAli((float) distance, diagonalLen);
                        } else {
                            evalue = evaluer.computeEvalue(distance, queryLen);
                            bitScore = static_cast<int>(evaluer.computeBitScore(distance) + 0.5);

                            if (par.rescoreMode == Parameters::RESCORE_MODE_ALIGNMENT) {
                                alnLen = (alignment.endPos - alignment.startPos) + 1;
                                int qStartPos, qEndPos, dbStartPos, dbEndPos;
                                // -1 since diagonal is computed from sequence Len which starts by 1
                                if (diagonal >= 0) {
                                    qStartPos = alignment.startPos + distanceToDiagonal;
                                    qEndPos = alignment.endPos + distanceToDiagonal;
                                    dbStartPos = alignment.startPos;
                                    dbEndPos = alignment.endPos;
                                } else {
                                    qStartPos = alignment.startPos;
                                    qEndPos = alignment.endPos;
                                    dbStartPos = alignment.startPos + distanceToDiagonal;
                                    dbEndPos = alignment.endPos + distanceToDiagonal;
                                }
//                                int qAlnLen = std::max(qEndPos - qStartPos, static_cast<int>(1));
//                                int dbAlnLen = std::max(dbEndPos - dbStartPos, static_cast<int>(1));
//                                seqId = (alignment.score1 / static_cast<float>(std::max(qAlnLength, dbAlnLength)))  * 0.1656 + 0.1141;

                                // compute seq.id if hit fulfills e-value but not by seqId criteria
                                if (evalue <= par.evalThr || isIdentity) {
                                    int idCnt = 0;
                                    for (int i = qStartPos; i <= qEndPos; i++) {
                                        idCnt += (querySeqToAlign[i] == targetSeq[dbStartPos + (i - qStartPos)]) ? 1 : 0;
                                    }
                                    seqId = Util::computeSeqId(par.seqIdMode, idCnt, queryLen, dbLen, alnLen);
                                }

                                char *end = Itoa::i32toa_sse2(alnLen, buffer);
                                size_t len = end - buffer;
                                std::string backtrace(buffer, len - 1);
                                backtrace.push_back('M');
                                queryCov = SmithWaterman::computeCov(qStartPos, qEndPos, queryLen);
                                targetCov = SmithWaterman::computeCov(dbStartPos, dbEndPos, dbLen);
                                if(isReverse){
                                    qStartPos = queryLen - qStartPos - 1;
                                    qEndPos = queryLen - qEndPos - 1;
                                }
                                result = Matcher::result_t(results[entryIdx].seqId, bitScore, queryCov, targetCov,
                                                           seqId, evalue, alnLen,
                                                           qStartPos, qEndPos, queryLen, dbStartPos, dbEndPos, dbLen,
                                                           backtrace);
                            }
                        }
                    }

                    //float maxSeqLen = std::max(static_cast<float>(targetLen), static_cast<float>(queryLen));
                    float currScorePerCol = static_cast<float>(distance) / static_cast<float>(diagonalLen);
                    // query/target cov mode
                    bool hasCov = Util::hasCoverage(par.covThr, par.covMode, queryCov, targetCov);
                    // --min-seq-id
                    bool hasSeqId = seqId >= (par.seqIdThr - std::numeric_limits<float>::epsilon());
                    bool hasEvalue = (evalue <= par.evalThr);
                    bool hasAlnLen = (alnLen >= par.alnLenThr);

                    // --filter-hits
                    bool hasToFilter = (par.filterHits == true && currScorePerCol >= scorePerColThr);
                    if (isIdentity || hasToFilter || (hasAlnLen && hasCov && hasSeqId && hasEvalue)) {
                        if (par.rescoreMode == Parameters::RESCORE_MODE_ALIGNMENT) {
                            alnResults.emplace_back(result);
                        } else if (par.rescoreMode == Parameters::RESCORE_MODE_SUBSTITUTION) {
                            hit_t hit;
                            hit.seqId = results[entryIdx].seqId;
                            hit.prefScore = bitScore;
                            hit.diagonal = diagonal;
                            shortResults.emplace_back(hit);
                        } else {
                            hit_t hit;
                            hit.seqId = results[entryIdx].seqId;
                            hit.prefScore = 100 * seqId;
                            hit.diagonal = diagonal;
                            shortResults.emplace_back(hit);
                        }
                    }
                }

                if (par.sortResults > 0 && alnResults.size() > 1) {
                    std::sort(alnResults.begin(), alnResults.end(), Matcher::compareHits);
                }
                for (size_t i = 0; i < alnResults.size(); ++i) {
                    size_t len = Matcher::resultToBuffer(buffer, alnResults[i], true, false);
                    resultBuffer.append(buffer, len);
                }

                if (par.sortResults > 0 && shortResults.size() > 1) {
                    std::sort(shortResults.begin(), shortResults.end(), hit_t::compareHitsByScoreAndId);
                }
                for (size_t i = 0; i < shortResults.size(); ++i) {
                    size_t len = snprintf(buffer, 100, "%u\t%d\t%d\n", shortResults[i].seqId, shortResults[i].prefScore,
                                          shortResults[i].diagonal);
                    resultBuffer.append(buffer, len);
                }

                resultWriter.writeData(resultBuffer.c_str(), resultBuffer.length(), queryKey, thread_idx);
                resultBuffer.clear();
                shortResults.clear();
                alnResults.clear();
            }
            if (reversePrefilterResult == true) {
                delete [] queryRevSeq;
            }
        }
        resultReader.remapData();
    }
    Debug(Debug::INFO) << "\nDone.\n";

    if (tDbrIdx != NULL) {
        delete tDbrIdx;
    }

    if (sameQTDB == false) {
        if(qDbrIdx != NULL){
            delete qDbrIdx;
        }
    }

    delete[] fastMatrix.matrix;
    delete[] fastMatrix.matrixData;
    delete subMat;
    return 0;
}

int rescorediagonal(int argc, const char **argv, const Command &command) {
    MMseqsMPI::init(argc, argv);
    Parameters &par = Parameters::getInstance();
    par.parseParameters(argc, argv, command, 4);


    Debug(Debug::INFO) << "Prefilter database: " << par.db3 << "\n";
    DBReader<unsigned int> resultReader(par.db3.c_str(), par.db3Index.c_str(), par.threads, DBReader<unsigned int>::USE_INDEX|DBReader<unsigned int>::USE_DATA);
    resultReader.open(DBReader<unsigned int>::LINEAR_ACCCESS);
    int dbtype = Parameters::DBTYPE_PREFILTER_RES;
    if(par.rescoreMode == Parameters::RESCORE_MODE_ALIGNMENT){
        dbtype = Parameters::DBTYPE_ALIGNMENT_RES;
    }
#ifdef HAVE_MPI
    size_t dbFrom = 0;
    size_t dbSize = 0;

    Util::decomposeDomainByAminoAcid(resultReader.getAminoAcidDBSize(), resultReader.getSeqLens(), resultReader.getSize(),
                                     MMseqsMPI::rank, MMseqsMPI::numProc, &dbFrom, &dbSize);
    std::pair<std::string, std::string> tmpOutput = Util::createTmpFileNames(par.db4, par.db4Index, MMseqsMPI::rank);

    DBWriter resultWriter(tmpOutput.first.c_str(), tmpOutput.second.c_str(), par.threads, par.compressed, dbtype);
    resultWriter.open();
    int status = doRescorediagonal(par, resultWriter, resultReader, dbFrom, dbSize);
    resultWriter.close();

    MPI_Barrier(MPI_COMM_WORLD);
    if(MMseqsMPI::rank == 0) {
        std::vector<std::pair<std::string, std::string>> splitFiles;
        for(int proc = 0; proc < MMseqsMPI::numProc; ++proc){
            std::pair<std::string, std::string> tmpFile = Util::createTmpFileNames(par.db4, par.db4Index, proc);
            splitFiles.push_back(std::make_pair(tmpFile.first,  tmpFile.second));
        }
        DBWriter::mergeResults(par.db4, par.db4Index, splitFiles);
    }
#else
    DBWriter resultWriter(par.db4.c_str(), par.db4Index.c_str(), par.threads, par.compressed, dbtype);
    resultWriter.open();
    int status = doRescorediagonal(par, resultWriter, resultReader, 0, resultReader.getSize());
    resultWriter.close();

#endif
    resultReader.close();
    return status;
}


