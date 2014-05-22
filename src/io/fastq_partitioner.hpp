/**
 * fastq_partitioner.hpp
 *
 *  Created on: Feb 18, 2014
 *      Author: tpan
 */

#ifndef FASTQ_PARTITIONER_HPP_
#define FASTQ_PARTITIONER_HPP_

#include <sstream>
#include <iterator>  // for ostream_iterator
#include <algorithm> // for copy.

#include "config.hpp"

#include "common/base_types.hpp"
#include "io/file_loader.hpp"

namespace bliss
{
  namespace io
  {

    ///// subclass of FileLoader.  using static polymorphism via CRTP.


    /**
     *  for coordinated distributed quality score reads to map back to original.
     *    short sequence - partition and parse at read boundaries.  in FASTQ format
     *    long sequence - partition in block aligned form and read.  in FASTA.   (up to 150B bases).
     *    older sequencers may have longer reads in FASTA.
     *
     *
     *  === assume files are relatively large.  simplify things by processing 1 file at a time.
     *
     * ids:
     *  file id.          (fid to filename map)
     *  read id in file   (read id to seq/qual offset map)
     *  position in read
     *
     *  ===  for reads, expected lengths are maybe up to 1K in length, and files contain >> 1 seq
     *
     *  issue:  can we fit the raw string into memory?  can we fit the result into memory?
     *    result has to be  able to fit.  N/P uint64_t.
     *    if result fits, the raw string has to fit.
     *
     *  1 pass algorithm: we can use offset as ids.
     *
     */
    template<typename T, bool Buffering = true, bool Preloading = false,
        typename ChunkPartitioner = bliss::partition::DemandDrivenPartitioner<size_t> >
    class FASTQLoader : public FileLoader<T, Buffering, Preloading, ChunkPartitioner,
                                          FASTQLoader<T, Buffering, Preloading, ChunkPartitioner> >
    {
      public:
#if defined(USE_MPI)
        explicit FASTQLoader(const std::string &_filename, const int _nThreads = 1, const size_t _chunkSize,
                    const MPI_Comm& _comm = MPI_COMM_SELF) throw (IOException)
            : FileLoader<T, Buffering, Preloading, ChunkPartitioner,
              FASTQLoader<T, Buffering, Preloading, ChunkPartitioner> >(_filename, _nThreads, _chunkSize, _comm)
#else
        explicit FASTQLoader(const std::string &_filename, const int _nThreads = 1, const size_t chunkSize,
                            const int _nProcs = 1, const int _rank = 0 ) throw (IOException)
            : FASTQLoader<T, Buffering, Preloading, ChunkPartitioner> >(_filename, _nThreads, _chunkSize, _nProcs, _rank)
#endif
        {
        };
        virtual ~FASTQLoader();

      protected:
        typedef FileLoader<T, Buffering, Preloading, ChunkPartitioner,
            FASTQLoader<T, Buffering, Preloading, ChunkPartitioner> >    SuperType;
        typedef typename SuperType::RangeType                             RangeType;
        typedef typename SuperType::SizeType                             SizeType;


        /**
         * search at the start and end of the block partition for some matching condition,
         * and set as new partition positions.
         */
        RangeType getNextPartitionRangeImpl() {
          RangeType hint = partitioner.getNext(rank);
          hint &= fileRange;
          SizeType length = hint.size();

          RangeType next = hint >> length;
          next &= fileRange;

          // concatenate current and next ranges
          RangeType searchRange = hint | next;
          searchRange.align_to_page(page_size);

          // map the content
          InputIteratorType searchData = this->map(searchRange) + searchRange.start - searchRange.block_start;

          // NOTE: assume that the range is large enough that we would not run into trouble with partition search not finding a starting position.

          // for output
          RangeType output(hint);

          // search for new start using finder
          output.start = findStart(searchData, fileRange, hint);

          // get the new ending offset
          output.end = findStart(searchData + length, fileRange, next);

          // clean up and unmap
          this->unmap(searchData, searchRange);

          // readjust
          return output;
        }

        RangeType getNextChunkRangeImpl(const int tid) {
          assert(loaded);
          RangeType hint = chunkPartitioner.getNext(tid);
          hint &= mmap_range;
          SizeType length = hint.size();

          RangeType next = hint >> length;
          next &= mmap_range;

          // chunk size is already set to at least 2x record size.

        }

        template <typename Finder2>
        RangeType adjustChunkRange(const RangeType &range, const Finder2 &finder) {
          assert(chunkSize >= (seqSize * 2));

          getSeqSize(partitioner, 3);


          SizeType cs = chunkSize;  // we need at least seqSize * 2
          SizeType s = mmap_range.end,
                   e = mmap_range.end;
          SizeType readLen = 0;


          //DEBUG("range " << range << " chunk " << cs << " chunkPos " << chunkPos);

          /// part that needs to be sequential
#pragma omp atomic capture
          { s = chunkPos; chunkPos += cs; }  // get the current value and then update

          if (s >= mmap_range.end) {
            printf("WARNING: should not be here often\n");
              s = mmap_range.end;
              readLen = 0;
          } else {
            // since we are just partitioning, need to search for start and for end.
            RangeType curr(s, s + cs);
            curr &= srcData.getRange();
            /// search start part.

            try {
              // search for start.
              s = partitioner(srcData.begin() + (curr.start - mmap_range.start), mmap_range, curr);
            } catch (IOException& ex) {
              // did not find the end, so set e to next.end.
              // TODO: need to handle this scenario better - should keep search until end.
              WARNING(ex.what());

              printf("curr range: chunk %lu, s %lu-%lu, curr %lu-%lu, srcData range %lu-%lu, aligned_range %lu-%lu\n",
                     cs, s, s+cs, curr.start, curr.end, srcData.getRange().start, srcData.getRange().end, mmap_range.start, mmap_range.end);
              printf("got an exception search for start:  %s \n", ex.what());
              s = curr.end;
            }

            RangeType next = curr >> cs;
            next &= srcData.getRange();
            /// search end part only.  update localPos for next search.
            //printf("next: %lu %lu\n", next.start, next.end);
            try {
              // search for end.
              e = partitioner(srcData.begin() + (next.start - mmap_range.start), mmap_range, next);
            } catch (IOException& ex) {
              // did not find the end, so set e to next.end.
              // TODO: need to handle this scenario better - should keep search until end.
              WARNING(ex.what());

              printf("next range: chunk %lu,  s %lu-%lu, curr %lu-%lu, srcData range %lu-%lu, aligned_range %lu-%lu\n",
                     cs, s, s+cs, curr.start, curr.end, srcData.getRange().start, srcData.getRange().end, mmap_range.start, mmap_range.end);

              printf("got an exception search for end: %s \n", ex.what());
              e = curr.end;
            }

            readLen = e - s;
            //printf("s = %lu, e = %lu, chunkPos = %lu, readLen 1 = %lu\n", s, e, chunkPos, readLen);
            //std::cout << "range: " << range << " next: " << next << std::endl;
          }

        }
          /**
           * search for first occurence of @ from an arbitrary starting point.  Specifically, we are looking for the pairing of @ and + separated by 1 line
           *    e.g. .*\n[@][^\n]*\n[ATCGN]+\n[+].*\n.*
           *    note that this combination can occur at 2 places only.  at beginning of read/sequence, or if read name contains @ and the partition happens to start there.
           *
           *    we can also look for  + followed by @ in 2 lines.
           *    e.g. [ATCGN]*\n[+][^\n]*\n[^\n]+\n[@].*\n.*
           *
           *    using just 1 of these patterns would require us to search through up to 7 lines.
           *      pattern 1 works better if partition starts on lines 3 or 4, and pattern 2 works better if partition starts on 1 or 2.
           *      either case need to look at 5 lines.
           *
           * standard pattern is @x*
           *                     l*
           *                     +x*
           *                     q*
           *                     @x*
           *                     ....
           *                  x could be @, +, or other char except "\n".
           *                  l is alphabet, so no @, +, or "\n"
           *                  q could be @, +, or other char within some ascii range., no "\n"
           *
           *                  block start on line 1:
           *                    @ at start, or @ at some unfortunate position in the middle of name.  + is unambiguous.
           *                        if first block (target.start=parent.start) we would have @ at start.
           *                        if partition is right at beginning of line 1, we can afford to skip to next line 1.
           *                            also takes care of partition in the middle of line 1.
           *                  block start on line 2:
           *                    can't have @ anywhere in this line.  not possible
           *                  block start on line 3:
           *                    @ at some unfortunate position in the middle of line 3.  line 5 = line 1 can't have + at start.  not possible
           *                  block start on line 4:
           *                    quality line unfortunately starts with @ or contains @.  line 6 == line 2 can't have + at start.  not possible.
           *
           *  Algorithm:  read 4 lines, not including the part before the first "\n". treat first partition as if it was preceded with a "\n"
           *      look through the saved results, and look for @..+ or +..@.
           *
           *  first block in the parent range is treated differently.
           *
           * @param _data     start of iterator.
           * @param parent    the "full" range to which the target range belongs.  used to determine if the target is a "first" block.
           * @param target    the target range in which to search for a record.  the start and end position in the file.
           * @return          position of the start of next read sequence (@).  if there is no complete sequence, return "end"
           */
          template<typename Iterator>
          const SizeType findStart(const Iterator &_data, const RangeType &parent, const RangeType &target) const
                                                         throw (bliss::io::IOException) {

            RangeType t = target & parent; // intersection to bound target to between parent's ends.
            if (t.start == t.end) return t.start;

            SizeType i = t.start;
            Iterator data(_data);
            bool wasEOL = false;

            ////// remove leading newlines
            while ((i < t.end) && (*data == '\n')) {
              wasEOL = true;
              ++data;
              ++i;
            }

            //// if no more, end.
            if (i == t.end)  // this part only contained \n
              return t.end;

            ///// if at beginning of parent partition, treat as if previous line was EOL
            if (t.start == parent.start) { // beginning of parent range, treat specially, since there is no preceeding \n for the @
                                                // all other partitions will lose the part before the first "\n@" (will be caught by the previous partition)
              wasEOL = true;
            }



            ///// now read 4 lines
            // already has the first line first char..
            ValueType first[4];
            SizeType offsets[4] =
            { t.end, t.end, t.end, t.end };   // units:  offset from beginning of file.

            int currLineId = 0;        // current line id in the buffer

            // read the rest of the lines.
            ValueType c;
            while ((i < t.end) && currLineId < 4)
            {
              c = *data;

              // encountered a newline.  mark newline found.
              if (c == '\n')
              {
                wasEOL = true;  // toggle on
              }
              else
              {
                if (wasEOL) // first char
                {
                  first[currLineId] = c;
                  offsets[currLineId] = i;
                  wasEOL = false;  // toggle off
                  ++currLineId;
                }
              }
              //    else  // other characters in the line - don't care.

              ++data;
              ++i;
            }

            //////////// determine the position of a read by looking for @...+ or +...@
            // at this point, first[0] is pointing to first char after first newline, or in the case of first block, the first char.
            // everything in "first" are right after newline.
            // ambiguity from quality line?  no.
            if (first[0] == '@' && first[2] == '+')  return offsets[0];
            if (first[1] == '@' && first[3] == '+')  return offsets[1];
            if (first[2] == '@' && first[0] == '+')  return offsets[2];
            if (first[3] == '@' && first[1] == '+')  return offsets[3];


            // is it an error not to find a fastq marker?
            std::stringstream ss;
            ss << "ERROR in file processing: file segment \n" << "\t\t"
               << t << "\n\t\t(original " << target << ")\n"
               << "\t\tdoes not contain valid FASTQ markers.\n String:";
            std::ostream_iterator<typename std::iterator_traits<Iterator>::value_type> oit(ss);
            std::copy(_data, _data + (target.end - target.start), oit);

            throw bliss::io::IOException(ss.str());

          }

          SizeType getRecordSizeImpl(int iterations) {

            //// TODO: if a different partitioner is used, seqSize may be incorrect.
            ////   seqSize is a property of the partitioner applied to the data.

            assert(loaded);

            SizeType s, e;
            SizeType ss = 0;
            RangeType parent(srcData.getRange());
            RangeType r(srcData.getRange());

            s = findStart(srcData.begin(), parent, r);

            for (int i = 0; i < iterations; ++i) {
              r.start = s + 1;   // advance by 1, in order to search for next entry.
              r &= parent;
              e = findStart(srcData.begin() + (r.start - parent.start), parent, r);
              ss = std::max(ss, (e - s));
              s = e;
            }

              // DEBUG("Sequence size is " << ss);
            return ss;
          }


    };

  } /* namespace io */
} /* namespace bliss */
#endif /* FASTQ_PARTITIONER_HPP_ */