#[test::annotation]
#[test::annotation(param, type, "string", 2 + 2)]
#[test::nested(foo(bar[baz{bat}]))]
export #[annotations] fn #[can] main(#[appear]) #[anywhere] void = {
	// no-op -- this is just a lexical test
	return;
};
