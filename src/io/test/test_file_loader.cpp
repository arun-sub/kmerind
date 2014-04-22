/**
 * file_loader_test.cpp
 *
 *  Created on: Feb 18, 2014
 *      Author: tpan
 */

// include google test
#include <gtest/gtest.h>
#include <cstdint>
#include <string>
#include <unistd.h>

#include "config.hpp"
#include "io/file_loader.hpp"
#include "iterators/range.hpp"


typedef bliss::io::file_loader<unsigned char> FileLoaderType;
typedef FileLoaderType::RangeType RangeType;

class FileLoaderTest : public ::testing::Test
{
  protected:
    virtual void SetUp()
    {
      fileName.assign(PROJ_SRC_DIR);
      fileName.append("/test/data/test.fastq");

      // get file size
      struct stat filestat;
      stat(fileName.c_str(), &filestat);
      fileSize = static_cast<size_t>(filestat.st_size);

      ASSERT_EQ(34111308, fileSize);
    }

    static void readFileC(const std::string &fileName, const size_t& offset,
                          const size_t& length, char* result)
    {
      FILE *fp = fopen(fileName.c_str(), "r");
      fseek(fp, offset, SEEK_SET);
      size_t read = fread_unlocked(result, sizeof(char), length, fp);
      fclose(fp);

      ASSERT_GT(read, 0);
    }

    std::string fileName;
    size_t fileSize;

};

// normal test cases
TEST_F(FileLoaderTest, OpenWithFullRange)
{
  // get fileName
  int rank = 0;
  int nprocs = 1;

  RangeType r =
      RangeType::block_partition(nprocs,
                                                          rank, 0, fileSize);

  FileLoaderType loader(fileName, r);

  size_t len = r.end - r.start;
  char* gold = new char[len];
  FileLoaderTest::readFileC(fileName, r.start, len, gold);

  int comp = memcmp(gold, loader.getData(), len * sizeof(char));
  ASSERT_EQ(0, comp);
  delete [] gold;
}
TEST_F(FileLoaderTest, PreloadWithFullRange)
{

  int rank = 0;
  int nprocs = 1;

  RangeType r =
      RangeType::block_partition(nprocs,
                                                          rank, 0, fileSize);

  FileLoaderType loader(fileName, r, true);

  size_t len = r.end - r.start;
  char* gold = new char[len];
  FileLoaderTest::readFileC(fileName, r.start, len, gold);

  int comp = memcmp(gold, loader.getData(), len * sizeof(char));
  ASSERT_EQ(0, comp);
  delete [] gold;
}
TEST_F(FileLoaderTest, OpenWithRange)
{
  int rank = 3;
  int nprocs = 7;

  RangeType r =
      RangeType::block_partition(nprocs,
                                                          rank, 0, fileSize);

  FileLoaderType loader(fileName, r);

  RangeType r2 = loader.getRange();
  size_t len = r2.end - r2.start;
  char* gold = new char[len];
  FileLoaderTest::readFileC(fileName, r2.start, len, gold);

  int comp = memcmp(gold, loader.getData(), len * sizeof(char));
  ASSERT_EQ(0, comp);
  delete [] gold;
}
TEST_F(FileLoaderTest, OpenWithAlignedRange)
{
  int rank = 3;
  int nprocs = 7;

  RangeType r =
      RangeType::block_partition(nprocs,
                                                          rank, 0, fileSize);
  RangeType ra = r.align_to_page(
      sysconf(_SC_PAGE_SIZE));
  FileLoaderType loader(fileName, ra);

  RangeType r2 = loader.getRange();
  size_t len = r2.end - r2.start;
  char* gold = new char[len];
  FileLoaderTest::readFileC(fileName, r2.start, len, gold);

  int comp = memcmp(gold, loader.getData(), len * sizeof(char));
  ASSERT_EQ(0, comp);
  delete [] gold;
}
TEST_F(FileLoaderTest, PreloadWithRange)
{

  int rank = 3;
  int nprocs = 7;

  RangeType r =
      RangeType::block_partition(nprocs,
                                                          rank, 0, fileSize);

  FileLoaderType loader(fileName, r, true);

  RangeType r2 = loader.getRange();
  size_t len = r2.end - r2.start;
  char* gold = new char[len];
  FileLoaderTest::readFileC(fileName, r2.start, len, gold);

  int comp = memcmp(gold, loader.getData(), len * sizeof(char));
  ASSERT_EQ(0, comp);
  delete [] gold;
}
TEST_F(FileLoaderTest, PreloadWithAlignedRange)
{

  int rank = 3;
  int nprocs = 7;

  RangeType r =
      RangeType::block_partition(nprocs,
                                                          rank, 0, fileSize);
  RangeType ra = r.align_to_page(
      sysconf(_SC_PAGE_SIZE));
  FileLoaderType loader(fileName, ra, true);

  RangeType r2 = loader.getRange();
  size_t len = r2.end - r2.start;
  char* gold = new char[len];
  FileLoaderTest::readFileC(fileName, r2.start, len, gold);

  int comp = memcmp(gold, loader.getData(), len * sizeof(char));
  ASSERT_EQ(0, comp);
  delete [] gold;
}

// TODO negative test cases
TEST_F(FileLoaderTest, NoFilename)
{
}
TEST_F(FileLoaderTest, BadFilename)
{
}
TEST_F(FileLoaderTest, EmptyFile)
{
}
TEST_F(FileLoaderTest, BadFileFormat)
{
}
TEST_F(FileLoaderTest, EmptyRange)
{
}
TEST_F(FileLoaderTest, BadRange)
{
}
