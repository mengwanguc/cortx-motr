package mds


import (
   "bytes"
    "testing"
    "motr/mio"
    ds "github.com/ipfs/go-datastore"

    dstest "github.com/ipfs/go-datastore/test"
)



// returns datastore, and a function to call on exit.
// So this will be:
//
//   d, done := newDS(t)
//   defer done()
func newDS(t *testing.T) (*MotrDS) {
    config := mio.Config{
        LocalEP:    "10.52.0.244@tcp:12345:33:1000",
        HaxEP:      "10.52.0.244@tcp:12345:34:1",
        Profile:    "0x7000000000000001:0",
        ProcFid:    "0x7200000000000001:64",
        TraceOn:    false,
        Verbose:    false,
        ThreadsN:   1,
    }
    indexID := "0x7800000000000001:123456701"
    motrds, err := Open(config, indexID)
    if err != nil {
        t.Fatal("Failed to open index.. error: ", err)
    }
    t.Cleanup(func() {
        motrds.Mkv.Close()
	})
    return motrds

}


func TestSuite(t *testing.T) {
    motrds := newDS(t)


    // test from go-datastore
    t.Run("BasicOperations", func(t *testing.T) {
        testBasicOperations(t, motrds)
    })

    t.Run("Query", func(t *testing.T) {
        testQuery(t, motrds)
    })

    t.Run("Batch", func(t *testing.T) {
        testBatch(t, motrds)
    })
    
}


func testBasicOperations(t *testing.T, motrds *MotrDS) {
	t.Run("Simple", func(t *testing.T) {
        testSimple(t, motrds)
    })

	// basic operation tests officially provided by go-datastore
	t.Run("BasicPutGet", func(t *testing.T) {
        dstest.SubtestBasicPutGet(t, motrds)
    })


    t.Run("NotFounds", func(t *testing.T) {
        dstest.SubtestNotFounds(t, motrds)
    })

    t.Run("BasicSync", func(t *testing.T) {
        dstest.SubtestBasicSync(t, motrds)
        // this test will add 2 key-values to the store, but doesn't clean it.
        // therefore we clean it manually after the test
        motrds.Delete(ds.NewKey("prefix"))
        motrds.Delete(ds.NewKey("prefix/sub"))
    })
}




func testQuery(t *testing.T, motrds *MotrDS) {
	t.Run("ManyKeysAndQuery", func(t *testing.T) {
        dstest.SubtestManyKeysAndQuery(t, motrds)
    })

    t.Run("Prefix", func(t *testing.T) {
        dstest.SubtestPrefix(t, motrds)
    })

    t.Run("Order", func(t *testing.T) {
        dstest.SubtestOrder(t, motrds)
    })

    t.Run("Limit", func(t *testing.T) {
        dstest.SubtestLimit(t, motrds)
    })

    t.Run("Filter", func(t *testing.T) {
        dstest.SubtestFilter(t, motrds)
    })

    t.Run("ReturnSizes", func(t *testing.T) {
        dstest.SubtestReturnSizes(t, motrds)
    })

}


func testBatch(t *testing.T, motrds ds.Batching) {
    t.Run("SimpleBatch", func(t *testing.T) {
        testSimpleBatch(t, motrds)
    })

    t.Run("BatchTest", func(t *testing.T) {
        dstest.RunBatchTest(t, motrds)
    })

    t.Run("BatchDeleteTest", func(t *testing.T) {
        dstest.RunBatchDeleteTest(t, motrds)
    })

}


// a simple test about batch
func testSimpleBatch(t *testing.T, motrds ds.Batching) {
    t.Log("putting 2 values to datastore in batch")

    k := ds.NewKey("foo")
    val := []byte("Hello Datastore!")


    k2 := ds.NewKey("ddd")
    val2 := []byte("Hello Datastore!")

    bt, err := motrds.Batch()
    bt.Put(k, val)
    bt.Put(k2, val)
    err = bt.Commit()

    if err != nil {
        t.Fatal("errr putting 2 keys to datastore in batch: ", err)
    }


    
    t.Log("getting and check the 2 values")


    getval, err := motrds.Get(k)

    if err != nil {
        t.Fatal("errr getting value of key foo from datastore: ", err)
    }

    if !bytes.Equal(getval, val) {
        t.Fatal("value received on get for key foo wasnt what we expected:", string(getval))
    }

    getval2, err := motrds.Get(k2)

    if err != nil {
        t.Fatal("errr getting value of key ddd from datastore: ", err)
    }

    if !bytes.Equal(getval2, val2) {
        t.Fatal("value received on get for key ddd wasnt what we expected:", string(getval2))
    }

    t.Log("deleting the 2 values in batch")


    bt.Delete(k)
    bt.Delete(k2)
    err = bt.Commit()

    if err != nil {
        t.Fatal("errr deleting the 2 values in batch: ", err)
    }


    have, err := motrds.Has(k)
    if err != nil {
        t.Fatal("error calling has on key foo after delete", err)
    }

    if have {
        t.Fatal("should deleted key foo, has returned true")
    }

    have, err = motrds.Has(k2)
    if err != nil {
        t.Fatal("error calling has on key ddd after delete", err)
    }

    if have {
        t.Fatal("should deleted key ddd, has returned true")
    }

}




// a simple test of operations
func testSimple(t *testing.T, motrds ds.Datastore) {

    // put
    k := ds.NewKey("foo")
    val := []byte("Hello Datastore!")

    err := motrds.Put(k, val)
    if err != nil {
        t.Fatal("error putting foo to datastore: ", err)
    }

    // get
    getval,err := motrds.Get(k)

    if err != nil {
        t.Fatal("errr getting value of key foo from datastore: ", err)
    }

    if !bytes.Equal(getval, val) {
        t.Fatal("value received on get for key foo wasnt what we expected:", string(getval))
    }

    //has
    have, err := motrds.Has(k)
    if err != nil {
        t.Fatal("error calling has on key foo ", err)
    }

    if !have {
        t.Fatal("should have key foo, has returned false")
    }

    // getsize
    size, err := motrds.GetSize(k)
    if err != nil {
        t.Fatal("error calling GetSize on key foo ", err)
    }

    if size != len(val) {
        t.Fatal("should have size ", len(val) , "GetSize returned ", size)
    }


    // check a key not in store
    newk := ds.NewKey("ddd")

    have, err = motrds.Has(newk)
    if err != nil {
        t.Fatal("error calling has on key ddd ", err)
    }

    if have {
        t.Fatal("shouldn't have key ddd, has returned true")
    }

    // delete
    err = motrds.Delete(k)
    if err != nil {
        t.Fatal("error calling delete on key foo ", err)
    }

    have, err = motrds.Has(k)
    if err != nil {
        t.Fatal("error calling has on key foo after delete", err)
    }

    if have {
        t.Fatal("should have deleted key foo, has returned true")
    }

}
